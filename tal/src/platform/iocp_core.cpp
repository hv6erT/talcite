#include "../netting.hpp"

#ifdef TAL_NET_IOCP

#include <winsock2.h>

#include <mswsock.h>
#include <windows.h>
#include <ws2tcpip.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "tal/executor.hpp"

// The reactor of Windows, ported from talcite's iocp.cpp: one thread for the whole program waits
// on a completion port. Windows says not that a socket is ready but that an operation is over, so
// every request starts its operation at once (`began`), and the thread finishes it when the
// system reports it (`ended`): a write goes on until all of it is out, an accept and a connect
// tell the system what the socket became. `Doing::done` wakes the task on its own executor; no
// code of a task runs on this thread.
namespace tal::net::netting {

namespace {

// The key of a completion that only wakes the thread to see that the reactor closes.
constexpr ULONG_PTR waking = 1;

// The room AcceptEx takes for each of the two addresses.
constexpr std::size_t roomy = sizeof(sockaddr_storage) + 16;

// An operation in flight, with all the system writes into until it ends: the overlapped
// structure, the buffer's description, the address, the counters.
struct Flying {
    OVERLAPPED over{};
    std::shared_ptr<Doing> one;
    WSABUF what{};
    sockaddr_storage told{};
    int wide = static_cast<int>(sizeof(sockaddr_storage));
    char space[2 * roomy]{};
    DWORD much = 0;
    DWORD marks = 0;
};

// The reactor: the operations in flight by their overlapped structure, how many each socket has,
// the sockets tied to the port, the sockets closed but not yet given back, whose new requests are
// refused, and the sockets handed over to be closed once nothing of theirs is in flight.
struct Reactor {
    Reactor();
    ~Reactor();

    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;

    std::mutex guard;
    HANDLE port = nullptr;
    std::unordered_map<OVERLAPPED*, std::shared_ptr<Flying>> flying;
    std::unordered_map<Handle, int> counted;
    std::unordered_set<Handle> tied;
    std::unordered_set<Handle> leaving;
    std::thread staff;
    std::size_t turns = 0;
    bool closing = false;
};

std::atomic<Reactor*> standing{nullptr};

LPFN_ACCEPTEX accepting = nullptr;
LPFN_CONNECTEX connecting = nullptr;

// Asks Winsock once for AcceptEx and ConnectEx, which have no entry of their own in the library.
void asking() {
    static std::once_flag once;

    std::call_once(once, [] {
        starting();

        const SOCKET one = ::socket(AF_INET, SOCK_STREAM, 0);

        if (one == INVALID_SOCKET) return;

        GUID which = WSAID_ACCEPTEX;
        DWORD much = 0;

        (void)::WSAIoctl(one, SIO_GET_EXTENSION_FUNCTION_POINTER, &which, sizeof which, &accepting, sizeof accepting, &much, nullptr, nullptr);

        GUID other = WSAID_CONNECTEX;

        (void)::WSAIoctl(one, SIO_GET_EXTENSION_FUNCTION_POINTER, &other, sizeof other, &connecting, sizeof connecting, &much, nullptr, nullptr);

        ::closesocket(one);
    });
}

// Ties the socket `one` to the port, once; 0, or the code why not. An operation on a socket the
// port does not know would end where nobody hears it.
int tying(Reactor& held, Handle one) {
    if (held.tied.contains(one)) return 0;

    if (::CreateIoCompletionPort(reinterpret_cast<HANDLE>(one), held.port, 0, 0) == nullptr) return static_cast<int>(::GetLastError());

    held.tied.insert(one);

    return 0;
}

// Closes the socket `one` once it was handed over and nothing of its is in flight: the system
// still writes into what an operation cancelled but not yet ended holds.
void sweeping(Reactor& held, Handle one) {
    if (const auto found = held.counted.find(one); found != held.counted.end() && found->second > 0) return;

    if (!held.leaving.contains(one)) return;

    held.leaving.erase(one);
    held.tied.erase(one);
    held.counted.erase(one);
    ending(one);
}

// Waits, on the io pool, for the connect of the local socket `one` that Windows does not end at
// once, nor through the port, since ConnectEx is not for local sockets: a failed connect shows in
// the set of exceptions, `SO_ERROR` says why, and a refusal may be a missing file (`refusal`).
void awaited_locally(std::shared_ptr<Doing> one) {
    io().post([one = std::move(one)] {
        fd_set writing;
        fd_set failing;

        FD_ZERO(&writing);
        FD_ZERO(&failing);
        FD_SET(static_cast<SOCKET>(one->held), &writing);
        FD_SET(static_cast<SOCKET>(one->held), &failing);

        const int ready = ::select(0, nullptr, &writing, &failing, nullptr);
        const int why = ready < 0 ? ::WSAGetLastError() : settling(one->held);

        one->done(0, why == WSAECONNREFUSED ? refusal(one->where) : why);
    });
}

// Starts the operation of `fly`; true when it is under way and its end comes through the port,
// false when it is over already, `done` told.
bool began(const std::shared_ptr<Flying>& fly) {
    Doing& one = *fly->one;
    const auto socket = static_cast<SOCKET>(one.held);

    switch (one.kind) {
        case Step::joining: {
            if (one.where.family() == Family::local) {
                sockaddr_storage told{};
                const int wide = filling(one.where, &told);

                if (::connect(socket, reinterpret_cast<sockaddr*>(&told), wide) == 0) {
                    one.done(0, 0);

                    return false;
                }

                const int why = ::WSAGetLastError();

                if (why != WSAEWOULDBLOCK) {
                    one.done(0, why == WSAECONNREFUSED ? refusal(one.where) : why);

                    return false;
                }

                awaited_locally(fly->one);

                return false;
            }

            asking();

            if (connecting == nullptr) {
                one.done(0, WSAEOPNOTSUPP);

                return false;
            }

            if (!bound(one.held, Address::any(one.where.family(), 0))) {
                one.done(0, amiss());

                return false;
            }

            const int wide = filling(one.where, &fly->told);

            if (!connecting(socket, reinterpret_cast<sockaddr*>(&fly->told), wide, nullptr, 0, &fly->much, &fly->over) &&
                ::WSAGetLastError() != ERROR_IO_PENDING) {
                one.done(0, ::WSAGetLastError());

                return false;
            }

            return true;
        }

        case Step::meeting: {
            asking();

            if (accepting == nullptr) {
                one.done(0, WSAEOPNOTSUPP);

                return false;
            }

            const Family family = named(one.held, false).family();

            one.made = family == Family::none ? nothing : opened(family, true);

            if (one.made == nothing) {
                one.done(0, amiss());

                return false;
            }

            if (!accepting(socket, static_cast<SOCKET>(one.made), fly->space, 0, static_cast<DWORD>(roomy), static_cast<DWORD>(roomy), &fly->much,
                           &fly->over) &&
                ::WSAGetLastError() != ERROR_IO_PENDING) {
                const int why = ::WSAGetLastError();

                ending(std::exchange(one.made, nothing));
                one.done(0, why);

                return false;
            }

            return true;
        }

        case Step::getting:
        case Step::fetching: {
            fly->what.buf = reinterpret_cast<CHAR*>(one.buffer.data());
            fly->what.len = static_cast<ULONG>(one.buffer.size());

            const int made = one.kind == Step::getting ? ::WSARecv(socket, &fly->what, 1, &fly->much, &fly->marks, &fly->over, nullptr)
                                                       : ::WSARecvFrom(socket, &fly->what, 1, &fly->much, &fly->marks,
                                                                       reinterpret_cast<sockaddr*>(&fly->told), &fly->wide, &fly->over, nullptr);

            if (made != 0 && ::WSAGetLastError() != WSA_IO_PENDING) {
                const int why = ::WSAGetLastError();

                if (one.kind == Step::fetching && why == WSAEMSGSIZE) {
                    one.where = facing(&fly->told);
                    one.cut = true;
                    one.done(one.buffer.size(), 0);

                    return false;
                }

                one.done(0, why);

                return false;
            }

            return true;
        }

        case Step::putting:
        case Step::sending: {
            fly->what.buf = reinterpret_cast<CHAR*>(one.buffer.data()) + one.much;
            fly->what.len = static_cast<ULONG>(one.buffer.size() - one.much);

            int made = 0;

            if (one.kind == Step::putting) {
                made = ::WSASend(socket, &fly->what, 1, &fly->much, 0, &fly->over, nullptr);
            } else {
                const int wide = filling(one.where, &fly->told);

                made = ::WSASendTo(socket, &fly->what, 1, &fly->much, 0, reinterpret_cast<sockaddr*>(&fly->told), wide, &fly->over, nullptr);
            }

            if (made != 0 && ::WSAGetLastError() != WSA_IO_PENDING) {
                one.done(one.much, ::WSAGetLastError());

                return false;
            }

            return true;
        }
    }

    return false;
}

// Puts the operation `fly` in flight on the reactor `held` and starts it, under the reactor's
// lock: a close then either refuses it here or cancels it after it started. A request on a closed
// socket, one whose awaiter is gone, and one on a socket the port refused are refused.
void launched(Reactor& held, const std::shared_ptr<Flying>& fly) {
    const Handle which = fly->one->held;
    int why = 0;

    {
        const std::lock_guard<std::mutex> kept{held.guard};

        if (fly->one->gone() || fly->one->closed()) {
            why = aborted();
        } else {
            why = tying(held, which);

            if (why == 0) {
                held.flying.emplace(&fly->over, fly);
                ++held.counted[which];

                if (began(fly)) return;

                held.flying.erase(&fly->over);
                --held.counted[which];
                sweeping(held, which);

                return;
            }
        }
    }

    fly->one->done(0, why);
}

// The operation of `fly` ended with `much` bytes and the code `code`, as Winsock says it. A cut
// datagram says so; an accept that failed closes its socket; a cancelled operation was taken back
// or its socket closed, which a request hears as a closed connection. A write not all out goes
// on.
void ended(Reactor& held, const std::shared_ptr<Flying>& fly, DWORD much, int code) {
    Doing& one = *fly->one;

    if (code == WSA_OPERATION_ABORTED) code = aborted();

    if (code != 0) {
        if (one.kind == Step::fetching && code == WSAEMSGSIZE) {
            one.cut = true;
            one.where = facing(&fly->told);
            one.done(much, 0);

            return;
        }

        if (one.kind == Step::meeting) ending(std::exchange(one.made, nothing));

        one.done(one.kind == Step::putting ? one.much : 0, code);

        return;
    }

    switch (one.kind) {
        case Step::joining:
            (void)::setsockopt(static_cast<SOCKET>(one.held), SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);
            one.done(0, 0);

            return;

        case Step::meeting: {
            const auto which = static_cast<SOCKET>(one.held);

            (void)::setsockopt(static_cast<SOCKET>(one.made), SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, reinterpret_cast<const char*>(&which),
                               sizeof which);
            one.where = named(one.made, true);
            one.done(0, 0);

            return;
        }

        case Step::fetching: one.where = facing(&fly->told); [[fallthrough]];
        case Step::getting:
        case Step::sending: one.done(much, 0); return;

        case Step::putting: {
            one.much += much;

            if (one.much >= one.buffer.size()) {
                one.done(one.much, 0);

                return;
            }

            auto next = std::make_shared<Flying>();

            next->one = fly->one;
            launched(held, next);

            return;
        }
    }
}

// Opens the port and starts the reactor's thread, which takes each completion out of the map under
// the lock and ends it outside it; the operation stops counting only then, since the socket must
// stay open while its end is read. Without a port there is no reactor, and requests are refused.
Reactor::Reactor() {
    starting();

    port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);

    if (port == nullptr) return;

    standing.store(this);

    staff = std::thread{[this] {
        for (;;) {
            DWORD much = 0;
            ULONG_PTR key = 0;
            OVERLAPPED* over = nullptr;
            const BOOL made = ::GetQueuedCompletionStatus(port, &much, &key, &over, INFINITE);

            if (over == nullptr || key == waking) {
                const std::lock_guard<std::mutex> kept{guard};

                if (closing) return;

                continue;
            }

            std::shared_ptr<Flying> fly;

            {
                const std::lock_guard<std::mutex> kept{guard};

                ++turns;

                if (const auto found = flying.find(over); found != flying.end()) {
                    fly = std::move(found->second);
                    flying.erase(found);
                }
            }

            if (!fly) continue;

            const Handle which = fly->one->held;
            int code = 0;

            if (!made) {
                DWORD moved = 0;
                DWORD marks = 0;

                code = ::WSAGetOverlappedResult(static_cast<SOCKET>(which), over, &moved, FALSE, &marks) ? 0 : ::WSAGetLastError();

                if (code == 0) code = static_cast<int>(::GetLastError());
            }

            ended(*this, fly, much, code);

            const std::lock_guard<std::mutex> kept{guard};

            if (const auto found = counted.find(which); found != counted.end() && found->second > 0) --found->second;

            sweeping(*this, which);
        }
    }};
}

// Stops the thread, and only then lets the world see no reactor. What is still in flight is
// cancelled and its ends collected for a while; the requests are let go, not refused, since the
// executors they would wake may be gone with the program. What the system never gave back stays
// allocated for ever, for the system may still write into it.
Reactor::~Reactor() {
    {
        const std::lock_guard<std::mutex> kept{guard};

        closing = true;
    }

    if (port != nullptr) (void)::PostQueuedCompletionStatus(port, 0, waking, nullptr);

    if (staff.joinable()) staff.join();

    standing.store(nullptr);

    for (const auto& [over, fly] : flying) (void)::CancelIoEx(reinterpret_cast<HANDLE>(fly->one->held), over);

    for (int turn = 0; turn < 100 && !flying.empty() && port != nullptr; ++turn) {
        DWORD much = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* over = nullptr;

        (void)::GetQueuedCompletionStatus(port, &much, &key, &over, 10);

        if (over == nullptr) continue;

        if (const auto found = flying.find(over); found != flying.end()) {
            if (found->second->one->kind == Step::meeting) ending(std::exchange(found->second->one->made, nothing));

            flying.erase(found);
        }
    }

    auto* stranded = new std::vector<std::shared_ptr<Flying>>;

    for (auto& [over, fly] : flying) stranded->push_back(std::move(fly));

    flying.clear();

    for (const Handle one : leaving) ending(one);

    if (port != nullptr) ::CloseHandle(port);
}

// The reactor, started with the first request.
Reactor* running() {
    static Reactor one;

    return standing.load();
}

}

// Starts the request `one`; its end comes through the port, or at once.
void submit(std::shared_ptr<Doing> one) {
    Reactor* held = running();

    if (held == nullptr || one->held == nothing) {
        one->done(0, aborted());

        return;
    }

    auto fly = std::make_shared<Flying>();

    fly->one = std::move(one);
    launched(*held, fly);
}

// Asks the system to cancel the request `one`, when its awaiter goes before it ended. The system
// only asks: the end still comes through the port, so nothing is taken back here.
bool withdraw(const std::shared_ptr<Doing>& one) {
    Reactor* held = standing.load();

    if (held == nullptr || !one || one->held == nothing) return false;

    const std::lock_guard<std::mutex> kept{held->guard};

    for (const auto& [over, fly] : held->flying)
        if (fly->one == one) {
            (void)::CancelIoEx(reinterpret_cast<HANDLE>(one->held), over);

            break;
        }

    return false;
}

// Hands the socket `one` over to be closed once nothing of its is in flight. Called with its last
// holder, so nobody starts anything on it any more.
void retiring(Handle one) {
    if (one == nothing) return;

    Reactor* held = standing.load();

    if (held == nullptr) {
        ending(one);

        return;
    }

    const std::lock_guard<std::mutex> kept{held->guard};

    held->leaving.insert(one);
    sweeping(*held, one);
}

// Cancels what is in flight on the socket `one`, whose ends come through the port, and shuts its
// conversation, which does not give its number back. The cancel is under the reactor's lock, so
// that an operation starting at the same time either sees the socket closed or is cancelled;
// without a reactor nothing is in flight.
void forget(Handle one) {
    if (Reactor* held = standing.load(); held != nullptr) {
        const std::lock_guard<std::mutex> kept{held->guard};

        (void)::CancelIoEx(reinterpret_cast<HANDLE>(one), nullptr);
    }

    shutting(one);
}

// How many sockets have something in flight now.
std::size_t watching() {
    Reactor* held = standing.load();

    if (held == nullptr) return 0;

    const std::lock_guard<std::mutex> kept{held->guard};
    std::size_t much = 0;

    for (const auto& [which, count] : held->counted)
        if (count > 0) ++much;

    return much;
}

// How many completions the reactor has taken.
std::size_t turns() {
    Reactor* held = standing.load();

    if (held == nullptr) return 0;

    const std::lock_guard<std::mutex> kept{held->guard};

    return held->turns;
}

}

#endif
