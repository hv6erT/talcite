#include "piping.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "talcite/pool.hpp"
#include "talcite/trouble.hpp"

#include "handing.hpp"
#include "starting.hpp"

#ifdef _WIN32

#include <winsock2.h>

#include <windows.h>

#include <sddl.h>

#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS 0x00000008
#endif

#endif

// Named pipes in place of local sockets. Windows has AF_UNIX only since 10 1803; earlier versions
// (7, 8, 8.1, the first releases of 10) and Wine do not. A local address stays a path, and the
// listener, the stream and the hub stay the same, but underneath there is a pipe
// \\.\pipe\talcite-… with a name computed from the path. The choice is made once, inside the
// library; the caller does not know about it.
//
// Each end of a pipe is a handle with overlapped I/O, bound to the hub's port like a socket:
// reading and writing are ReadFile and WriteFile, accepting is ConnectNamedPipe (iocp.cpp), and
// connecting is `CreateFileW` in the pool, because a pipe has no overlapped connect. Pipe handles
// are ordinary hub handles (`Handle`); a registry says which handle is a pipe and on which side,
// because closing, naming and the peer work differently for a pipe than for a socket.
//
// The name comes from the path: \\.\pipe\talcite-<path hash>-<file name>. The hash takes slashes in
// one form and ignores letter case, since Windows does not distinguish them in paths, so both sides
// that gave the same path reach the same pipe.
//
// Only us: the pipe has an access list with just the user who created it, and it rejects remote
// clients. Pipe names are shared by the whole machine, so someone could create a pipe with this
// name before us; that is why the listener's first instance demands to be the first one (otherwise
// the listener refuses), and a side that cares asks about its peer (`Stream::kindred`). The client
// does not let the server impersonate it (`SECURITY_IDENTIFICATION`).
namespace talcite::net::pipes {

#ifdef _WIN32

namespace {

constexpr DWORD roomy = 64 * 1024;

struct End {
    std::string path;
    bool server = false;
};

// The registry lives until the end of the process: the hub still closes handles while statics are
// being destroyed.
struct Ends {
    std::mutex guard;
    std::unordered_map<Handle, End> all;
};

// The registry of pipe ends, created once and never destroyed.
Ends& ends() {
    static auto* held = new Ends;

    return *held;
}

// Registers a pipe handle as an end on `path`, the server's or the client's, and returns it as a
// hub handle.
Handle enrolled(HANDLE one, const std::string& path, bool server) {
    const Handle made = reinterpret_cast<Handle>(one);

    const std::lock_guard<std::mutex> kept{ends().guard};

    ends().all[made] = End{path, server};

    return made;
}

// What the registry knows about the handle, or nothing when it is not a pipe.
std::optional<End> end_of(Handle one) {
    const std::lock_guard<std::mutex> kept{ends().guard};

    const auto found = ends().all.find(one);

    if (found == ends().all.end()) return std::nullopt;

    return found->second;
}

// UTF-8 converted to UTF-16 for the wide Windows calls.
std::wstring wide(const std::string& one) {
    if (one.empty()) return {};

    const int much = MultiByteToWideChar(CP_UTF8, 0, one.data(), static_cast<int>(one.size()), nullptr, 0);

    std::wstring made(static_cast<std::size_t>(much), L'\0');

    MultiByteToWideChar(CP_UTF8, 0, one.data(), static_cast<int>(one.size()), made.data(), much);

    return made;
}

// The pipe name for a path: a 64-bit FNV-1a hash of the path with slashes unified and letters
// lowered, followed by the file name cut to 64 characters, with everything outside [A-Za-z0-9._-]
// replaced by an underscore.
std::wstring pipe_name(const std::string& path) {
    std::uint64_t sum = 14695981039346656037ull;

    for (char one : path) {
        if (one == '/') one = '\\';

        if (one >= 'A' && one <= 'Z') one = static_cast<char>(one - 'A' + 'a');

        sum ^= static_cast<unsigned char>(one);
        sum *= 1099511628211ull;
    }

    static constexpr char digits[] = "0123456789abcdef";

    std::string made = "\\\\.\\pipe\\talcite-";

    for (int shift = 60; shift >= 0; shift -= 4) made += digits[(sum >> shift) & 0xf];

    const std::size_t cut = path.find_last_of("\\/");
    std::string tail = cut == std::string::npos ? path : path.substr(cut + 1);

    if (tail.size() > 64) tail.resize(64);

    std::replace_if(
        tail.begin(), tail.end(),
        [](char one) {
            return !((one >= 'a' && one <= 'z') || (one >= 'A' && one <= 'Z') || (one >= '0' && one <= '9') || one == '.' ||
                     one == '_' || one == '-');
        },
        '_');

    if (!tail.empty()) made += "-" + tail;

    return wide(made);
}

// An "only me" access list: SDDL with the SID of the process's user. Null when it cannot be built;
// the listener frees it with LocalFree.
PSECURITY_DESCRIPTOR owner_only() {
    HANDLE token = nullptr;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == 0) return nullptr;

    DWORD much = 0;

    (void)GetTokenInformation(token, TokenUser, nullptr, 0, &much);

    std::string room(much, '\0');
    PSECURITY_DESCRIPTOR made = nullptr;

    if (much > 0 && GetTokenInformation(token, TokenUser, room.data(), much, &much) != 0) {
        const auto* told = reinterpret_cast<const TOKEN_USER*>(room.data());
        LPWSTR sid = nullptr;

        if (ConvertSidToStringSidW(told->User.Sid, &sid) != 0) {
            const std::wstring said = L"D:P(A;;GA;;;" + std::wstring{sid} + L")";

            (void)ConvertStringSecurityDescriptorToSecurityDescriptorW(said.c_str(), SDDL_REVISION_1, &made, nullptr);

            LocalFree(sid);
        }
    }

    CloseHandle(token);

    return made;
}

}

// A pipe listener: the name is taken with the first instance, and from then on there is always
// exactly one listening instance (the free one or the one an accept waits on), so that the name
// does not disappear between accepts.
struct Gate {
    // Closes the free instance and releases the access list.
    ~Gate() {
        pipes::shut(*this);

        if (rights != nullptr) LocalFree(rights);
    }

    // A new instance with the same name and rights. The first one must be the first at all: a name
    // taken by someone else is a refusal, not joining someone else's pipe.
    [[nodiscard]] HANDLE instance(bool first) {
        SECURITY_ATTRIBUTES how{};

        how.nLength = sizeof how;
        how.lpSecurityDescriptor = rights;

        return CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES,
                                roomy, roomy, 0, rights != nullptr ? &how : nullptr);
    }

    // `free` is an instance without a waiting accept. There is always at most one listening
    // instance (the free one or the one an accept waits on): a client lands in any listening
    // instance, so if a second one lay next to the waiting one, a client in it would wait for an
    // accept that looks elsewhere. Other clients get "busy" and wait (`dialed`) until an instance
    // is free.
    std::mutex guard;
    std::wstring name;
    std::string path;
    PSECURITY_DESCRIPTOR rights = nullptr;
    Handle free = nothing;
    Handle pending = nothing;
    bool closing = false;
};

// True where a local address goes through a pipe: Windows without AF_UNIX (probed once by opening
// such a socket) or a build with `TALCITE_PIPES`.
bool chosen() {
#if defined(TALCITE_PIPES)
    return true;
#elif defined(TALCITE_IOCP)
    static const bool made = [] {
        starting();

        const SOCKET one = ::socket(AF_UNIX, SOCK_STREAM, 0);

        if (one == INVALID_SOCKET) return true;

        ::closesocket(one);

        return false;
    }();

    return made;
#else
    // The portable backend waits for socket readiness, and a pipe does not report readiness.
    return false;
#endif
}

// Whether the handle is a pipe end.
bool piped(Handle one) { return one != nothing && end_of(one).has_value(); }

// Whether the handle is a server end, one that came from a listener.
bool served(Handle one) {
    const std::optional<End> found = end_of(one);

    return found && found->server;
}

// The local address of a pipe end (`peer` false) or of its peer (`peer` true). As with AF_UNIX,
// the server end knows the path as its own, the client as its peer's.
Address named(Handle one, bool peer) {
    const std::optional<End> found = end_of(one);

    if (!found || found->server == peer) return {};

    try {
        return Address::local(found->path);
    } catch (const Trouble&) {
        return {};
    }
}

// The process ID on the other side of the pipe, or nothing.
std::optional<unsigned long> facing(Handle one) {
    const std::optional<End> found = end_of(one);

    if (!found) return std::nullopt;

    ULONG pid = 0;
    const HANDLE pipe = reinterpret_cast<HANDLE>(one);
    const BOOL told = found->server ? GetNamedPipeClientProcessId(pipe, &pid) : GetNamedPipeServerProcessId(pipe, &pid);

    if (told == 0) return std::nullopt;

    return static_cast<unsigned long>(pid);
}

// Removes the end from the registry and closes its handle.
void closed(Handle one) {
    {
        const std::lock_guard<std::mutex> kept{ends().guard};

        ends().all.erase(one);
    }

    CloseHandle(reinterpret_cast<HANDLE>(one));
}

// Opens a listener on the local address `where`; throws `Trouble` when the name cannot be taken.
std::shared_ptr<Gate> gated(const Address& where) {
    auto made = std::make_shared<Gate>();

    made->path = where.path();
    made->name = pipe_name(where.path());
    made->rights = owner_only();

    const HANDLE first = made->instance(true);

    if (first == INVALID_HANDLE_VALUE) {
        const int why = static_cast<int>(GetLastError());

        throw Trouble{"Cannot listen: " + spoken(why) + ".", why, where};
    }

    // A second guard against a taken name: a system that does not enforce the first instance (Wine)
    // still tells how many instances the pipe has.
    DWORD instances = 0;

    if (GetNamedPipeHandleStateW(first, nullptr, &instances, nullptr, nullptr, nullptr, 0) != 0 && instances > 1) {
        CloseHandle(first);

        throw Trouble{"Cannot listen: " + spoken(WSAEADDRINUSE) + ".", WSAEADDRINUSE, where};
    }

    made->free = enrolled(first, made->path, true);

    return made;
}

// An instance to wait for a client on (the free one or a new one); nothing once the listener is
// closing.
Handle spare(Gate& gate) {
    const std::lock_guard<std::mutex> kept{gate.guard};

    if (gate.closing) return nothing;

    Handle made = gate.free;

    if (made == nothing) {
        const HANDLE one = gate.instance(false);

        if (one == INVALID_HANDLE_VALUE) return nothing;

        made = enrolled(one, gate.path, true);
    }

    gate.free = nothing;
    gate.pending = made;

    return made;
}

// The accept on `done` has ended (in whatever way), so the listener gets a new free instance, so
// that the name does not disappear and a client has somewhere to land before the next accept,
// unless the next one already waits on its own. That happens after a cancellation: the place in
// the listener frees at once, and the completion of the cancelled instance comes back only later.
void refill(Gate& gate, Handle done) {
    const std::lock_guard<std::mutex> kept{gate.guard};

    if (gate.pending != done) return;

    gate.pending = nothing;

    if (gate.closing || gate.free != nothing) return;

    const HANDLE one = gate.instance(false);

    if (one != INVALID_HANDLE_VALUE) gate.free = enrolled(one, gate.path, true);
}

// Closes the listener. The free instance has never been in the hub, so it closes at once.
void shut(Gate& gate) {
    const std::lock_guard<std::mutex> kept{gate.guard};

    gate.closing = true;

    if (gate.free != nothing) closed(gate.free);

    gate.free = nothing;
}

namespace {

struct DialPromise : detail::Result<Stream> {};

}

// Connects to the pipe of `where` in the pool. When all instances are busy, we wait for a free one
// in slices, so that cancellation and the deadline do not wait for the system; a missing pipe is a
// refused connection, as with a socket without a listener. A stream nobody wants any more
// (cancelled at the last moment) goes away together with its handle.
Promise<Stream> dialed(Address where, Patience patience) {
    auto one = std::make_shared<DialPromise>();
    const bool ever = patience.count() > 0;
    const auto until = std::chrono::steady_clock::now() + patience;

    pooling().post([one, where, ever, until] {
        const std::wstring name = pipe_name(where.path());

        for (;;) {
            if (one->quitted.load()) return;

            const HANDLE made = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);

            if (made != INVALID_HANDLE_VALUE) {
                settled(*one, greeting(enrolled(made, where.path(), false), where));

                return;
            }

            const DWORD why = GetLastError();

            if (why == ERROR_PIPE_BUSY) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();

                if (ever && left <= 0) {
                    spoiled(*one, std::make_exception_ptr(Overdue{}));

                    return;
                }

                (void)WaitNamedPipeW(name.c_str(), static_cast<DWORD>(ever ? std::min<long long>(left, 250) : 250));

                continue;
            }

            const int code = why == ERROR_FILE_NOT_FOUND ? WSAECONNREFUSED : static_cast<int>(why);

            spoiled(*one, std::make_exception_ptr(Trouble{"Cannot connect: " + spoken(code) + ".", code, where}));

            return;
        }
    });

    return HandPromise<Stream>{one};
}

#else

struct Gate {};

// Without Windows a local address never goes through a pipe.
bool chosen() { return false; }

// Without Windows no handle is a pipe.
bool piped(Handle) { return false; }

// Without Windows no handle is a pipe's server end.
bool served(Handle) { return false; }

// Without Windows a pipe end has no address.
Address named(Handle, bool) { return {}; }

// Without Windows there is no process across a pipe.
std::optional<unsigned long> facing(Handle) { return std::nullopt; }

void closed(Handle) {}

// Throws `Trouble`: named pipes exist only on Windows.
std::shared_ptr<Gate> gated(const Address& where) { throw Trouble{"Named pipes are a Windows thing.", 0, where}; }

// Without Windows there is never an instance to wait on.
Handle spare(Gate&) { return nothing; }

void refill(Gate&, Handle) {}

void shut(Gate&) {}

// A promise rejected with `Trouble`: named pipes exist only on Windows.
Promise<Stream> dialed(Address where, Patience) {
    auto made = std::make_shared<detail::Result<Stream>>();

    detail::spoiled(*made, std::make_exception_ptr(Trouble{"Named pipes are a Windows thing.", 0, std::move(where)}));

    return HandPromise<Stream>{made};
}

#endif

}
