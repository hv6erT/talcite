#include "exchange.hpp"

#include "inside.hpp"

#ifdef TALCITE_IOCP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "talcite/pool.hpp"

#include "piping.hpp"
#include "starting.hpp"

#include <winsock2.h>

#include <mswsock.h>
#include <windows.h>
#include <ws2tcpip.h>

namespace talcite::net {

namespace {

// The pipe is closed on the other side. For the reader this is the end of the stream, not a
// failure.
bool ended_pipe(DWORD code) { return code == ERROR_BROKEN_PIPE || code == ERROR_PIPE_NOT_CONNECTED; }

// A completion key that belongs to no operation; it only means "wake up and check the
// deadlines". It replaces the socket-pair alarm that the WSAPoll backend needs.
constexpr ULONG_PTR waking = 1;

constexpr std::size_t roomy = sizeof(sockaddr_storage) + 16;

// An operation in flight. The OVERLAPPED must outlive the whole system call, and the kernel
// writes to the buffer long after the function returns, so all of this is owned by the exchange
// and not by a closure that is about to disappear.
struct Flying {
    OVERLAPPED over{};
    std::shared_ptr<Doing> one;
    WSABUF what{};
    sockaddr_storage told{};
    int wide = static_cast<int>(sizeof(sockaddr_storage));
    char space[2 * roomy]{};

    // The byte counter and the flags must outlive the call too. The function that starts the
    // operation returns as soon as the kernel says "pending", so they cannot live on its stack.
    // MSDN says it plainly: with a non-null lpOverlapped this pointer must be null or outlive the
    // operation.
    DWORD much = 0;
    DWORD marks = 0;

    bool late = false;
};

LPFN_ACCEPTEX accepting = nullptr;
LPFN_CONNECTEX joining = nullptr;

// AcceptEx and ConnectEx have no ordinary library entry point; they have to be requested by GUID
// from an open socket. This is done once, on a socket created only for that purpose.
void asking() {
    static std::once_flag once;

    std::call_once(once, [] {
        starting();

        const SOCKET one = ::socket(AF_INET, SOCK_STREAM, 0);

        if (one == INVALID_SOCKET) return;

        GUID which = WSAID_ACCEPTEX;
        DWORD much = 0;

        (void)::WSAIoctl(one, SIO_GET_EXTENSION_FUNCTION_POINTER, &which, sizeof which,
                         &accepting, sizeof accepting, &much, nullptr, nullptr);

        GUID other = WSAID_CONNECTEX;

        (void)::WSAIoctl(one, SIO_GET_EXTENSION_FUNCTION_POINTER, &other, sizeof other, &joining,
                         sizeof joining, &much, nullptr, nullptr);

        ::closesocket(one);
    });
}

}

struct Exchange::Held {
    mutable std::mutex guard;
    HANDLE port = nullptr;
    std::unordered_map<OVERLAPPED*, std::shared_ptr<Flying>> flying;
    std::unordered_map<Handle, int> counted;
    std::unordered_set<Handle> tied;
    std::unordered_set<Handle> leaving;
    std::thread staff;
    bool closing = false;
};

namespace {

std::atomic<Exchange::Held*> standing{nullptr};

// Binds a handle to the completion port. A socket is bound once and for all, and binding it again
// fails, so we remember which handles are already bound.
//
// Zero means "bound"; anything else is an error code. The caller must not start the operation on
// failure: its completion would have nowhere to go, because the port knows nothing about this
// handle. The `Ticket` would stay fresh, a thread in `await` would never wake up, and the program
// would never finish exiting.
//
// The code comes from `GetLastError`, not from `amiss()`, because this is a Win32 call, not a
// Winsock one.
int tying(Exchange::Held& held, Handle one) {
    if (held.tied.count(one) != 0) return 0;

    if (::CreateIoCompletionPort(reinterpret_cast<HANDLE>(one), held.port, 0, 0) == nullptr)
        return static_cast<int>(::GetLastError());

    held.tied.insert(one);

    return 0;
}

// Closes a handle that is waiting to be closed, but only once no completion for it is still
// outstanding. CancelIoEx does not end an operation immediately; the kernel ends it later and
// delivers a completion with ERROR_OPERATION_ABORTED. A handle closed earlier would leave the
// kernel writing into freed memory.
void sweeping(Exchange::Held& held, Handle one) {
    if (const auto found = held.counted.find(one); found != held.counted.end() && found->second > 0)
        return;

    if (held.leaving.count(one) == 0) return;

    held.leaving.erase(one);
    held.tied.erase(one);
    held.counted.erase(one);

    ending(one);
}

// Delivers a completion to the operation on a pool thread, not here. The promise's continuation is
// foreign code; if it ran on the exchange thread, one slow then would stall the whole network.
void handed(std::shared_ptr<Doing> one, std::size_t much, int code, bool late) {
    pooling().post([one = std::move(one), much, code, late]() mutable {
        // A write interrupted by its deadline is not a success, even if some bytes went out: an
        // interrupted 1 MB write of which 64 KiB was sent must not report itself as complete.
        if (late && (much == 0 || one->kind == Kind::putting)) {
            one->late();

            return;
        }

        one->done(much, late ? 0 : code);
    });
}

// Starts an operation. On Windows every operation starts here: there is no "ready to read"
// signal, only "this much was read for you". True means "started, wait for the completion";
// false means the request is already settled.
bool began(const std::shared_ptr<Flying>& fly) {
    Doing& one = *fly->one;

    switch (one.kind) {
        case Kind::joining: {
            asking();

            if (joining == nullptr) {
                handed(fly->one, 0, WSAEOPNOTSUPP, false);

                return false;
            }

            // A local socket does not go through ConnectEx, and this is the proper path, not a
            // workaround. ConnectEx needs a socket that is already bound, and a local address has
            // no "any" form that could be bound: `Address::any` refuses it, rightly, because such
            // an address does not exist.
            //
            // More importantly, a connection on the same machine has nothing to wait for: there
            // is no handshake and no network. Overlapped I/O adds nothing here except a dependence
            // on whether Winsock exposes ConnectEx for AF_UNIX at all, which the documentation
            // does not say.
            //
            // So the connection is attempted on the spot and we return false, meaning "the request
            // is already handled".
            if (one.where.family() == Family::local) {
                sockaddr_storage told{};

                const int wide = filling(one.where, &told);

                if (::connect(static_cast<SOCKET>(one.held), reinterpret_cast<sockaddr*>(&told),
                              wide) == 0) {
                    handed(fly->one, 0, 0, false);

                    return false;
                }

                const int why = ::WSAGetLastError();

                // Anything other than "not yet" is a final answer: no such file, nobody
                // listening, no permission.
                if (why != WSAEWOULDBLOCK) {
                    handed(fly->one, 0, why, false);

                    return false;
                }

                // Measured on Windows: a local connection does not complete immediately. The
                // socket is non-blocking, so `connect` answers `WSAEWOULDBLOCK` and completes a
                // moment later.
                //
                // The port cannot be used for this, because without ConnectEx nobody posts a
                // completion there. So we wait on a pool thread, which is the right place: the
                // pool for ordinary work grows without limit precisely because such work is
                // allowed to wait. The wait is a `select` for writability, the same thing the
                // portable backend does, only for one socket instead of all of them.
                //
                // The request is not in the exchange's map (we return false), so the exchange
                // thread does not watch its deadline, and the wait itself has to measure it.
                const Handle which = one.held;
                const bool ever = one.ever;
                const auto until = one.until;

                pooling().post([mine = fly->one, which, ever, until]() {
                    timeval how{};

                    if (ever) {
                        const auto left = until - std::chrono::steady_clock::now();
                        const auto much =
                            std::chrono::duration_cast<std::chrono::milliseconds>(left).count();
                        const std::int64_t kept = much < 0 ? 0 : much;

                        how.tv_sec = static_cast<long>(kept / 1000);
                        how.tv_usec = static_cast<long>((kept % 1000) * 1000);
                    }

                    fd_set writing;
                    fd_set failing;

                    FD_ZERO(&writing);
                    FD_ZERO(&failing);
                    FD_SET(static_cast<SOCKET>(which), &writing);
                    FD_SET(static_cast<SOCKET>(which), &failing);

                    // Windows reports a failed connection in the exception set, not in the write
                    // set, so we wait on both at once.
                    const int ready = ::select(0, nullptr, &writing, &failing, ever ? &how : nullptr);

                    if (ready == 0) {
                        handed(mine, 0, 0, true);

                        return;
                    }

                    if (ready < 0) {
                        handed(mine, 0, ::WSAGetLastError(), false);

                        return;
                    }

                    // Readiness only says "it is over"; whether it succeeded is told by
                    // `SO_ERROR`.
                    handed(mine, 0, settling(which), false);
                });

                return false;
            }

            // ConnectEx requires a socket that is already bound; otherwise it refuses.
            if (!bound(one.held, Address::any(one.where.family(), 0))) {
                handed(fly->one, 0, amiss(), false);

                return false;
            }

            const int wide = filling(one.where, &fly->told);

            const BOOL made =
                joining(static_cast<SOCKET>(one.held), reinterpret_cast<sockaddr*>(&fly->told),
                        wide, nullptr, 0, &fly->much, &fly->over);

            if (!made && ::WSAGetLastError() != ERROR_IO_PENDING) {
                handed(fly->one, 0, ::WSAGetLastError(), false);

                return false;
            }

            return true;
        }

        case Kind::meeting: {
            // A pipe waits for a client on its own instance (`pipes::spare`); the accepted
            // instance is the stream itself. A client that connected before us produces no
            // completion on the port, so that case is settled on the spot. A client that has
            // also already left is a stream that says "end" right away.
            if (pipes::piped(one.held)) {
                if (::ConnectNamedPipe(reinterpret_cast<HANDLE>(one.held), &fly->over) != 0) return true;

                const DWORD why = ::GetLastError();

                if (why == ERROR_IO_PENDING) return true;

                if (why == ERROR_PIPE_CONNECTED || why == ERROR_NO_DATA) {
                    one.made = one.held;

                    handed(fly->one, 0, 0, false);

                    return false;
                }

                handed(fly->one, 0, static_cast<int>(why), false);

                return false;
            }

            asking();

            if (accepting == nullptr) {
                handed(fly->one, 0, WSAEOPNOTSUPP, false);

                return false;
            }

            const Family family = named(one.held, false).family();

            one.made = family == Family::none ? nothing : opened(family, true);

            if (one.made == nothing) {
                handed(fly->one, 0, amiss(), false);

                return false;
            }

            const BOOL made = accepting(static_cast<SOCKET>(one.held),
                                        static_cast<SOCKET>(one.made), fly->space, 0,
                                        static_cast<DWORD>(roomy), static_cast<DWORD>(roomy),
                                        &fly->much, &fly->over);

            if (!made && ::WSAGetLastError() != ERROR_IO_PENDING) {
                const int why = ::WSAGetLastError();

                ending(one.made);

                one.made = nothing;

                handed(fly->one, 0, why, false);

                return false;
            }

            return true;
        }

        case Kind::getting:
        case Kind::fetching: {
            if (pipes::piped(one.held)) {
                if (::ReadFile(reinterpret_cast<HANDLE>(one.held), one.buffer.data(), static_cast<DWORD>(one.buffer.size()), nullptr,
                               &fly->over) != 0)
                    return true;

                const DWORD why = ::GetLastError();

                if (why == ERROR_IO_PENDING) return true;

                handed(fly->one, 0, ended_pipe(why) ? 0 : static_cast<int>(why), false);

                return false;
            }

            fly->what.buf = reinterpret_cast<CHAR*>(one.buffer.data());
            fly->what.len = static_cast<ULONG>(one.buffer.size());

            const int made =
                one.kind == Kind::getting
                    ? ::WSARecv(static_cast<SOCKET>(one.held), &fly->what, 1, &fly->much, &fly->marks,
                                &fly->over, nullptr)
                    : ::WSARecvFrom(static_cast<SOCKET>(one.held), &fly->what, 1, &fly->much, &fly->marks,
                                    reinterpret_cast<sockaddr*>(&fly->told), &fly->wide,
                                    &fly->over, nullptr);

            if (made != 0 && ::WSAGetLastError() != WSA_IO_PENDING) {
                const int why = ::WSAGetLastError();

                // A datagram larger than the buffer is not a failure but a truncation, and, as on
                // Unix, we say so explicitly instead of handing back a piece that pretends to be
                // the whole.
                if (one.kind == Kind::fetching && why == WSAEMSGSIZE) {
                    one.where = facing(&fly->told);
                    one.cut = true;

                    handed(fly->one, one.buffer.size(), 0, false);

                    return false;
                }

                handed(fly->one, 0, why, false);

                return false;
            }

            return true;
        }

        case Kind::putting:
        case Kind::sending: {
            if (pipes::piped(one.held)) {
                // A zero-byte write reaches the reader as zero bytes, that is, as the end of the
                // stream. A socket sends nothing in that case, and neither does the pipe.
                if (one.much >= one.buffer.size()) {
                    handed(fly->one, one.much, 0, false);

                    return false;
                }

                if (::WriteFile(reinterpret_cast<HANDLE>(one.held), one.buffer.data() + one.much,
                                static_cast<DWORD>(one.buffer.size() - one.much), nullptr, &fly->over) != 0)
                    return true;

                const DWORD why = ::GetLastError();

                if (why == ERROR_IO_PENDING) return true;

                handed(fly->one, one.much, static_cast<int>(why), false);

                return false;
            }

            fly->what.buf = reinterpret_cast<CHAR*>(one.buffer.data()) + one.much;
            fly->what.len = static_cast<ULONG>(one.buffer.size() - one.much);

            int made = 0;

            if (one.kind == Kind::putting) {
                made = ::WSASend(static_cast<SOCKET>(one.held), &fly->what, 1, &fly->much, 0,
                                 &fly->over, nullptr);
            } else {
                const int wide = filling(one.where, &fly->told);

                made = ::WSASendTo(static_cast<SOCKET>(one.held), &fly->what, 1, &fly->much, 0,
                                   reinterpret_cast<sockaddr*>(&fly->told), wide, &fly->over,
                                   nullptr);
            }

            if (made != 0 && ::WSAGetLastError() != WSA_IO_PENDING) {
                handed(fly->one, one.much, ::WSAGetLastError(), false);

                return false;
            }

            return true;
        }
    }

    return false;
}

// A completion came back from the kernel. This decides whether the operation is finished or has to
// be issued again: a write may go out in pieces, and an accept or a connect needs the system told
// what the socket has just become.
void ended(const std::shared_ptr<Flying>& fly, DWORD much, int code) {
    Doing& one = *fly->one;

    // The end of a pipe while reading: as many bytes as arrived, and no error, exactly like a
    // closed socket.
    if (code != 0 && one.kind == Kind::getting && ended_pipe(static_cast<DWORD>(code)) && pipes::piped(one.held)) code = 0;

    if (code != 0) {
        // A datagram that is too large is not a receive failure but an answer: this much fit,
        // the rest is lost. It has to be handled here as well as on the immediate path, or a
        // receive issued before anything arrived would report a bare error code and lose both the
        // received bytes and the sender's address.
        if (one.kind == Kind::fetching && (code == WSAEMSGSIZE || code == ERROR_MORE_DATA)) {
            one.cut = true;
            one.where = facing(&fly->told);

            handed(fly->one, much, 0, fly->late);

            return;
        }

        if (one.kind == Kind::meeting && one.made != nothing) {
            ending(one.made);

            one.made = nothing;
        }

        handed(fly->one, one.kind == Kind::putting ? one.much : 0, code, fly->late);

        return;
    }

    switch (one.kind) {
        case Kind::joining: {
            // Without this, getpeername and getsockname know nothing on a freshly connected
            // socket.
            const SOCKET which = static_cast<SOCKET>(one.held);

            (void)::setsockopt(which, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);

            handed(fly->one, 0, 0, false);

            return;
        }

        case Kind::meeting: {
            if (pipes::piped(one.held)) {
                one.made = one.held;

                handed(fly->one, 0, 0, false);

                return;
            }

            const SOCKET which = static_cast<SOCKET>(one.held);

            (void)::setsockopt(static_cast<SOCKET>(one.made), SOL_SOCKET,
                               SO_UPDATE_ACCEPT_CONTEXT, reinterpret_cast<const char*>(&which),
                               sizeof which);

            one.where = named(one.made, true);

            handed(fly->one, 0, 0, false);

            return;
        }

        case Kind::fetching: one.where = facing(&fly->told); [[fallthrough]];

        case Kind::getting: handed(fly->one, much, 0, fly->late); return;

        case Kind::sending: handed(fly->one, much, 0, fly->late); return;

        // A write is finished only when everything has gone out. The rest goes in a second
        // request, because the buffer and the counter are still where they were.
        case Kind::putting: {
            one.much += much;

            if (one.much >= one.buffer.size()) {
                handed(fly->one, one.much, 0, false);

                return;
            }

            // An interrupted write is not a success, even if part of it went out: half a packet
            // cannot be handed back to the caller the way read bytes are.
            if (fly->late) {
                handed(fly->one, 0, 0, true);

                return;
            }

            submit(fly->one);

            return;
        }
    }
}

}

// Creates the completion port and starts the exchange thread. The thread waits on the port with a
// timeout set by the nearest deadline, hands each completion to `ended`, and cancels operations
// whose deadline has passed.
Exchange::Exchange() : held_(std::make_unique<Held>()) {
    // The pool must exist before the exchange, even though nothing here needs it yet. Static
    // objects are destroyed in reverse order of construction, and the exchange thread hands work
    // to the pool; a pool created later would be destroyed earlier, and the thread would reach
    // into freed memory.
    (void)pooling();

    starting();

    held_->port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);

    standing = held_.get();

    held_->staff = std::thread{[this] {
        Held& held = *held_;

        for (;;) {
            std::chrono::steady_clock::time_point soonest{};
            bool timed = false;

            {
                const std::lock_guard<std::mutex> kept{held.guard};

                if (held.closing) break;

                for (const auto& [over, fly] : held.flying)
                    if (fly->one->ever && !fly->late && (!timed || fly->one->until < soonest)) {
                        soonest = fly->one->until;
                        timed = true;
                    }
            }

            DWORD patience = INFINITE;

            if (timed) {
                // Round up, not down, for the same reason as in the poll backend: truncating to
                // zero for a deadline less than a millisecond away makes the port return
                // immediately, the sweep finds nothing overdue, and the loop spins on a full core.
                const auto left = std::chrono::ceil<std::chrono::milliseconds>(
                    soonest - std::chrono::steady_clock::now());

                // And clamp to the range: `INFINITE` is `0xFFFFFFFF`, so after the cast a deadline
                // more than roughly forty-nine days away would mean "wait forever", that is, it
                // would never fire.
                const std::int64_t much =
                    std::min<std::int64_t>(std::max<std::int64_t>(left.count(), 0),
                                           static_cast<std::int64_t>(INFINITE) - 1);

                patience = static_cast<DWORD>(much);
            }

            DWORD much = 0;
            ULONG_PTR key = 0;
            OVERLAPPED* over = nullptr;

            const BOOL made = ::GetQueuedCompletionStatus(held.port, &much, &key, &over, patience);

            const int why = made ? 0 : static_cast<int>(::GetLastError());

            // No OVERLAPPED means nothing came back: the time ran out or the port failed. Either
            // way the deadlines have to be checked.
            if (over != nullptr && key != waking) {
                std::shared_ptr<Flying> fly;

                {
                    const std::lock_guard<std::mutex> kept{held.guard};

                    if (const auto found = held.flying.find(over); found != held.flying.end()) {
                        fly = found->second;

                        held.flying.erase(found);
                    }

                    if (fly) {
                        if (const auto found = held.counted.find(fly->one->held);
                            found != held.counted.end() && found->second > 0)
                            --found->second;

                        sweeping(held, fly->one->held);
                    }
                }

                if (fly) ended(fly, much, why);
            }

            // Overdue operations are cancelled one operation at a time, not per socket: one
            // stream may carry a read with a deadline and a write without one. The completion
            // comes back anyway, with the number of bytes that got through, and that number must
            // not be lost.
            std::vector<std::pair<Handle, OVERLAPPED*>> cutting;

            {
                const std::lock_guard<std::mutex> kept{held.guard};

                if (held.closing) break;

                const auto now = std::chrono::steady_clock::now();

                for (auto& [which, fly] : held.flying)
                    if (fly->one->ever && !fly->late && fly->one->until <= now) {
                        fly->late = true;

                        cutting.emplace_back(fly->one->held, which);
                    }
            }

            for (const auto& [which, over_one] : cutting)
                (void)::CancelIoEx(reinterpret_cast<HANDLE>(which), over_one);
        }
    }};
}

// Tears the exchange down. The steps come in a fixed order and each has a reason.
Exchange::~Exchange() {
    {
        const std::lock_guard<std::mutex> kept{held_->guard};

        held_->closing = true;
    }

    // 1. The exchange thread stops; from now on nobody hands work to the pool.
    if (held_->port != nullptr) ::PostQueuedCompletionStatus(held_->port, 0, waking, nullptr);

    if (held_->staff.joinable()) held_->staff.join();

    // 2. The exchange disappears for the rest of the world, before the pool is drained, because a
    //    task finishing in the pool may submit itself again, and with the thread stopped nobody
    //    would serve it.
    standing.store(nullptr);

    // 3. The pool finishes what it was given.
    pooling().drain();

    // 4. Operations still pending. They must not simply be freed: the kernel holds pointers to
    //    their OVERLAPPED, buffers and addresses, and abandoned ones would leave it writing into
    //    freed memory. So we cancel them first and collect the completions that are about to
    //    come back.
    {
        std::vector<Handle> which;

        for (const auto& [over, fly] : held_->flying) which.push_back(fly->one->held);

        for (const Handle one : which) (void)::CancelIoEx(reinterpret_cast<HANDLE>(one), nullptr);
    }

    // Each request is also the result of the caller's promise, so every one that passes through
    //    here must be settled. An abandoned one leaves the `Ticket` fresh, a thread waiting in
    //    `await()` never wakes up, and the program never finishes exiting. The POSIX backend does
    //    the same at the same point (`exchange.cpp`, `done(0, ECONNABORTED)`).
    //
    //    We call `done()` directly, not through `ended()`. The latter goes through `handed()`,
    //    which posts the continuation to the pool, and the pool was drained one step earlier and
    //    we are in static destruction. POSIX calls `done()` directly for the same reason.
    const auto aborted = [](const std::shared_ptr<Flying>& fly) {
        Doing& one = *fly->one;

        // A rejected accept would leave open the socket it was supposed to become; the normal
        // path closes it at the same point.
        if (one.kind == Kind::meeting && one.made != nothing) {
            ending(one.made);

            one.made = nothing;
        }

        one.done(0, WSAECONNABORTED);
    };

    for (int turn = 0; turn < 1000 && !held_->flying.empty() && held_->port != nullptr; ++turn) {
        DWORD much = 0;
        ULONG_PTR whose = 0;
        OVERLAPPED* over = nullptr;

        if (::GetQueuedCompletionStatus(held_->port, &much, &whose, &over, 10) == 0 &&
            over == nullptr)
            continue;

        if (over == nullptr) continue;

        const auto at = held_->flying.find(over);

        if (at != held_->flying.end()) {
            aborted(at->second);

            held_->flying.erase(at);
        }
    }

    // Whatever the kernel did not return in that time will not come back anyway, and then we keep
    // those operations alive instead of freeing memory out from under the kernel. A leak at
    // program exit is a lesser evil than a write into a freed buffer.
    //
    // The leak must be real, that is, never freed. A function-local static does not give that: it
    // is created during static destruction, so it would be destroyed right after we return from
    // here, bringing back exactly the write into freed memory it was meant to prevent. Hence a
    // heap pointer that nobody ever reclaims.
    //
    // Settling the promise frees nothing, so the two do not conflict: the caller gets an answer
    // and the buffers stay with the kernel.
    auto* stranded = new std::vector<std::shared_ptr<Flying>>;

    for (auto& [over, fly] : held_->flying) {
        aborted(fly);

        stranded->push_back(fly);
    }

    held_->flying.clear();

    for (const Handle one : held_->leaving) ending(one);

    if (held_->port != nullptr) ::CloseHandle(held_->port);
}

// The number of handles that still have at least one operation in flight.
std::size_t Exchange::waiting() const {
    const std::lock_guard<std::mutex> kept{held_->guard};

    std::size_t much = 0;

    for (const auto& [which, count] : held_->counted)
        if (count > 0) ++much;

    return much;
}

// The single exchange of the program, created on first use.
Exchange& Exchange::running() {
    static Exchange one;

    return one;
}

// Registers an operation with the exchange and starts it. If the exchange is already gone, the
// operation is aborted with WSAECONNABORTED; if the handle cannot be bound to the port, it is
// settled with that error.
void submit(std::shared_ptr<Doing> one) {
    (void)Exchange::running();

    Exchange::Held* held = standing.load();

    if (held == nullptr) {
        one->done(0, WSAECONNABORTED);

        return;
    }

    auto fly = std::make_shared<Flying>();

    fly->one = std::move(one);

    OVERLAPPED* over = &fly->over;
    const Handle which = fly->one->held;

    int why = 0;

    // The entry goes in before the operation starts, because the completion can come back before
    // the system call returns, and then we would be looking for an operation not yet in the map.
    {
        const std::lock_guard<std::mutex> kept{held->guard};

        held->flying.emplace(over, fly);
        ++held->counted[which];

        // Binding to the port touches the exchange's map, so it has to happen here, under the
        // lock, and not in began, which runs without it.
        why = tying(*held, which);
    }

    // An unbound handle is not started. Its completion would have nowhere to go, so the operation
    // would run in the kernel while the promise waited for it forever.
    if (why == 0 && began(fly)) {
        // Without this the exchange would sleep until the previous deadline, or forever when it
        // was empty, so a fresh deadline would never pass.
        if (fly->one->ever)
            (void)::PostQueuedCompletionStatus(held->port, 0, waking, nullptr);

        return;
    }

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        held->flying.erase(over);

        if (const auto found = held->counted.find(which);
            found != held->counted.end() && found->second > 0)
            --found->second;

        sweeping(*held, which);
    }

    // When `began` returned false, the request is already settled; it did that itself. When the
    // binding failed, nobody did, because `began` never ran, so it is settled here.
    if (why != 0) handed(fly->one, 0, why, false);
}

// Withdraws one request. Here it looks different from Unix and ends differently.
//
// There the request sits in a map and taking it out is enough: nothing will ever touch it. Here
// the operation is already running in the kernel, and `CancelIoEx` only asks for it to be
// interrupted; the completion still arrives at the port, with ERROR_OPERATION_ABORTED, and only
// that completion clears the direction flag. So the result is always false: we removed nothing,
// and the caller must not clean up after itself. Telling the waiter the right thing is the job of
// `Ticket::quitted`, not of this function.
//
// Scanning `flying` linearly is deliberate. There are as many entries as operations in flight, and
// cancellation is not a hot path; a separate map would cost an entry on every submission to speed
// up a rare event.
bool withdraw(const std::shared_ptr<Doing>& one) {
    Exchange::Held* held = standing.load();

    if (held == nullptr || !one || one->held == nothing) return false;

    OVERLAPPED* over = nullptr;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        for (const auto& [which, fly] : held->flying)
            if (fly->one == one) {
                over = which;

                break;
            }
    }

    if (over != nullptr) (void)::CancelIoEx(reinterpret_cast<HANDLE>(one->held), over);

    return false;
}

// Closes a descriptor through the exchange. It is called only when the last handle to the socket
// dies. Completions in flight hold the handle through `kept`, so while any of them lives we do not
// get here; the system hands the number of a closed socket to the next one immediately.
void retiring(Handle one) {
    if (one == nothing) return;

    Exchange::Held* held = standing.load();

    if (held == nullptr) {
        ending(one);

        return;
    }

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        held->leaving.insert(one);

        sweeping(*held, one);
    }
}

// Forgetting a socket cancels everything pending on it. It does not close the descriptor; that is
// the job of retiring.
void forget(Handle one) {
    Exchange::Held* held = standing.load();

    if (held == nullptr) return;

    (void)::CancelIoEx(reinterpret_cast<HANDLE>(one), nullptr);

    shutting(one);
}

}

#endif
