#include "inside.hpp"
#include "sealed.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

#include "talcite/address.hpp"

#include "piping.hpp"
#include "starting.hpp"

#ifdef _WIN32
#include <winsock2.h>

#include <ws2tcpip.h>

#include <afunix.h>
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

// The system side of the network layer. inside.hpp picks the backend, so that every file agrees on
// the same one: IOCP on Windows, epoll on Linux, and the portable poll path elsewhere.
// `TALCITE_POLL` forces the portable path everywhere; without it the fallback backend could not be
// tested on a machine that happens to have something better.
//
// A `Doing` is a request waiting in the hub. The hub performs its step (it alone knows how to do
// that on the given system) and the request only receives the result: so many bytes, this error
// number. Reading, accepting and sending therefore know nothing about poll, epoll or IOCP, and a
// new backend is added without touching any of them.
//
// `withdraw` (defined by each backend) removes exactly one request, without touching the socket or
// the other direction. It is not `forget`, which removes the whole socket, breaks the conversation
// and drops everything that was pending on it. True means the request really left and neither
// `done` nor `late` will run on it any more; only then may the caller clean up after itself.
namespace talcite::net {

// The largest byte count that may be passed in one call where the system takes the length as an
// int.
constexpr int brimful = 1 << 30;


namespace {

// A talcite address holds sixteen raw bytes, while the system wants its own structure. The
// conversion lives in one place so that nobody rewrites it their own way, and so that only this
// file knows what a sockaddr looks like. Returns the length of the filled structure, or 0 for an
// empty address.
int filled(const Address& from, sockaddr_storage& into) {
    std::memset(&into, 0, sizeof into);

    if (from.family() == Family::four) {
        auto* four = reinterpret_cast<sockaddr_in*>(&into);

        four->sin_family = AF_INET;
        four->sin_port = htons(from.port());

        std::memcpy(&four->sin_addr, from.bytes().data(), 4);

        return static_cast<int>(sizeof(sockaddr_in));
    }

    // A local socket has neither an address nor a port but a path, and the structure length is
    // computed from the path length, not from the field size. The path fits together with its zero
    // terminator, because `Address::local` enforces that when the address is built, so there is
    // nothing to check here a second time.
    if (from.family() == Family::local) {
        auto* nearby = reinterpret_cast<sockaddr_un*>(&into);

        nearby->sun_family = AF_UNIX;

        const std::string& said = from.path();

        std::memcpy(nearby->sun_path, said.data(), said.size());

        return static_cast<int>(offsetof(sockaddr_un, sun_path) + said.size() + 1);
    }

    // An empty address is not IPv6. A default-constructed Address must not turn into a valid
    // sockaddr_in6 for [::]:0, on which binding would succeed instead of being refused.
    if (from.family() != Family::six) return 0;

    auto* six = reinterpret_cast<sockaddr_in6*>(&into);

    six->sin6_family = AF_INET6;
    six->sin6_port = htons(from.port());
    six->sin6_scope_id = from.scope();

    std::memcpy(&six->sin6_addr, from.bytes().data(), 16);

    return static_cast<int>(sizeof(sockaddr_in6));
}

// The reverse of `filled`: turns a system address into a talcite one. An unknown family gives an
// empty address.
Address seen(const sockaddr_storage& from) {
    std::array<std::byte, 16> raw{};

    if (from.ss_family == AF_INET) {
        const auto* four = reinterpret_cast<const sockaddr_in*>(&from);

        std::memcpy(raw.data(), &four->sin_addr, 4);

        return Address{Family::four, raw, ntohs(four->sin_port)};
    }

    // Our end of a local connection has no name: `getsockname` then returns an empty `sun_path`,
    // and an empty address is the truth here, not a failure.
    if (from.ss_family == AF_UNIX) {
        const auto* nearby = reinterpret_cast<const sockaddr_un*>(&from);

        std::size_t much = 0;

        while (much < sizeof nearby->sun_path && nearby->sun_path[much] != '\0') ++much;

        if (much == 0) return {};

        // Linux allows a path a few bytes longer than our address can hold. We cannot express
        // someone else's socket with such a path, so we return an empty address instead of throwing
        // from a helper that sits on the path of every completion.
        try {
            return Address::local(std::string{nearby->sun_path, much});
        } catch (const Trouble&) {
            return {};
        }
    }

    if (from.ss_family == AF_INET6) {
        const auto* six = reinterpret_cast<const sockaddr_in6*>(&from);

        std::memcpy(raw.data(), &six->sin6_addr, 16);

        Address made{Family::six, raw, ntohs(six->sin6_port)};

        made.scope(six->sin6_scope_id);

        return made;
    }

    return {};
}

}

// The Windows backend builds and takes apart addresses on its own, so these two are exported rather
// than keeping a second copy of that knowledge. `filling` fills a sockaddr_storage and returns its
// length.
int filling(const Address& from, void* into) {
    return filled(from, *static_cast<sockaddr_storage*>(into));
}

// Reads a sockaddr_storage given as an untyped pointer into a talcite address.
Address facing(const void* from) { return seen(*static_cast<const sockaddr_storage*>(from)); }

// Which side of the socket the given operation waits for. Connecting completes when the socket is
// ready for writing, which is the only non-obvious entry in this table.
bool outward(Kind kind) {
    return kind == Kind::joining || kind == Kind::putting || kind == Kind::sending;
}

// One step of an operation. This is the only place that knows what the work looks like on this
// system; the request gets nothing from it but a byte count and an error number. Returns true when
// the operation has finished; false means "the system could not do it yet, keep waiting".
bool stepped(Doing& one) {
    switch (one.kind) {
        case Kind::joining: one.done(0, settling(one.held)); return true;

        case Kind::meeting: {
            Handle made = nothing;

            const Address from = taken(one.held, made);

            if (made == nothing) {
                if (again()) return false;

                one.done(0, amiss());

                return true;
            }

            one.made = made;
            one.where = from;

            one.done(0, 0);

            return true;
        }

        case Kind::getting: {
            const long much = getting(one.held, one.buffer.data(), one.buffer.size());

            if (much < 0) {
                if (again()) return false;

                one.done(0, amiss());

                return true;
            }

            one.done(static_cast<std::size_t>(much), 0);

            return true;
        }

        // Writing finishes only once everything has gone out; between one turn and the next the
        // request remembers how much has been sent.
        case Kind::putting: {
            while (one.much < one.buffer.size()) {
                const long much =
                    putting(one.held, one.buffer.data() + one.much, one.buffer.size() - one.much);

                if (much < 0) {
                    if (again()) return false;

                    one.done(one.much, amiss());

                    return true;
                }

                one.much += static_cast<std::size_t>(much);
            }

            one.done(one.much, 0);

            return true;
        }

        case Kind::fetching: {
            bool cut = false;

            const long much =
                fetching(one.held, one.buffer.data(), one.buffer.size(), one.where, cut);

            if (much < 0) {
                if (again()) return false;

                one.done(0, amiss());

                return true;
            }

            one.cut = cut;

            one.done(static_cast<std::size_t>(much), 0);

            return true;
        }

        case Kind::sending: {
            const long much = sending(one.held, one.buffer.data(), one.buffer.size(), one.where);

            if (much < 0) {
                if (again()) return false;

                one.done(0, amiss());

                return true;
            }

            one.done(static_cast<std::size_t>(much), 0);

            return true;
        }
    }

    return true;
}

// Opens a socket of the given family, stream or datagram, or returns `nothing`. Sockets are
// non-blocking from the start, because the hub never wants to wait inside a system call: waiting
// happens in one place, which watches all sockets at once.
Handle opened(Family family, bool stream) {
    starting();

    // A local socket is stream-only. Windows has no datagrams for it at all, and a family that can
    // do more on one system than on another is worse than one that can do the same everywhere.
    if (family == Family::local && !stream) return nothing;

    const int which = family == Family::local  ? AF_UNIX
                      : family == Family::four ? AF_INET
                                               : AF_INET6;

#ifdef _WIN32
    const Handle made = ::socket(which, stream ? SOCK_STREAM : SOCK_DGRAM, 0);

    if (made == nothing) return nothing;

    u_long yes = 1;

    ::ioctlsocket(static_cast<SOCKET>(made), FIONBIO, &yes);

    // An ICMP "port unreachable" must not break a UDP socket. Windows reports it to the next read
    // as WSAECONNRESET, so a single packet sent to a dead address (a peer that has left, an old
    // STUN server) would cut off reception from everyone else. On POSIX an unconnected socket never
    // gets it. SIO_UDP_CONNRESET is _WSAIOW(IOC_VENDOR, 12), written as a number because it lives
    // in mswsock.h, which nothing else needs.
    if (!stream) {
        BOOL off = FALSE;
        DWORD given = 0;

        (void)::WSAIoctl(static_cast<SOCKET>(made), 0x9800000CUL, &off, sizeof off, nullptr, 0, &given, nullptr,
                         nullptr);
    }
#else
    // Closed when a child program is started. Otherwise the child inherits all our sockets and
    // keeps the port busy long after we close it; this is the same reason the Windows branch builds
    // a list of handles to pass on.
    const Handle made = sealed::socketed(which, stream ? SOCK_STREAM : SOCK_DGRAM);

    if (made < 0) return nothing;

    // Writing to a socket whose other side has already closed must not kill our process. The
    // default reaction to SIGPIPE is death, and a network library that kills the caller's program
    // because the peer hung up is unusable. Where MSG_NOSIGNAL exists, we switch it off on every
    // send; BSD and macOS have no such send flag, so they get an option on the socket.
    //
    // The process-wide setting stays untouched: SIG_IGN for SIGPIPE would silently change the
    // conditions of the whole application, which nobody asked for. The same rule holds for the
    // pipes of a child program, where the signal is blocked only while writing and only on our own
    // thread.
#ifdef SO_NOSIGPIPE
    {
        int yes = 1;

        ::setsockopt(made, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
    }
#endif
#endif

    // Listening on `::` must also accept IPv4 clients. Windows does not do this by default, so the
    // same code would listen on two different sets of addresses on two systems.
    if (family == Family::six) {
        int off = 0;

        ::setsockopt(static_cast<int>(made), IPPROTO_IPV6, IPV6_V6ONLY,
                     reinterpret_cast<const char*>(&off), sizeof off);
    }

    return made;
}

// Closes a socket or a named pipe; `nothing` is ignored.
void ending(Handle one) {
    if (one == nothing) return;

    if (pipes::piped(one)) return pipes::closed(one);

#ifdef _WIN32
    ::closesocket(static_cast<SOCKET>(one));
#else
    ::close(one);
#endif
}

// Starts connecting to the given address. Returns true only when the connection completed at once;
// otherwise the caller asks `underway` whether it is still in progress.
bool joined(Handle one, const Address& where) {
    sockaddr_storage told{};

    const int much = filled(where, told);

    const int made = ::connect(static_cast<int>(one), reinterpret_cast<sockaddr*>(&told), much);

    // Zero means it completed immediately, which is the rule on loopback, not the exception.
    return made == 0;
}

// A non-blocking connect completes with readiness for writing, but only SO_ERROR tells whether it
// succeeded. Without this question a failed connection looks like a successful one. Returns the
// error number, 0 on success.
int settling(Handle one) {
    int why = 0;

#ifdef _WIN32
    int much = sizeof why;
#else
    socklen_t much = sizeof why;
#endif

    if (::getsockopt(static_cast<int>(one), SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char*>(&why), &much) != 0)
        return amiss();

    return why;
}

// The address of our end (`peer` false) or of the other end (`peer` true); empty when the system
// cannot tell. Named pipes answer through `pipes::named`.
Address named(Handle one, bool peer) {
    if (pipes::piped(one)) return pipes::named(one, peer);

    sockaddr_storage told{};

#ifdef _WIN32
    int much = sizeof told;
#else
    socklen_t much = sizeof told;
#endif

    const int made = peer ? ::getpeername(static_cast<int>(one),
                                          reinterpret_cast<sockaddr*>(&told), &much)
                          : ::getsockname(static_cast<int>(one),
                                          reinterpret_cast<sockaddr*>(&told), &much);

    if (made != 0) return {};

    return seen(told);
}

// Without this a server cannot start again for a minute after closing, because the port sits in
// TIME_WAIT. On Windows SO_REUSEADDR means something else (it allows taking over someone else's
// port), so it is not set there.
void sharing(Handle one) {
#ifndef _WIN32
    const int yes = 1;

    ::setsockopt(one, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
#else
    (void)one;
#endif
}

// Allows sending to the broadcast address (a beacon in the local network). Without this permission
// the system refuses to send to 255.255.255.255, so that nobody broadcasts by accident.
void broadcasting(Handle one, bool given) {
    const int yes = given ? 1 : 0;

#ifdef _WIN32
    ::setsockopt(static_cast<SOCKET>(one), SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof yes);
#else
    ::setsockopt(one, SOL_SOCKET, SO_BROADCAST, &yes, sizeof yes);
#endif
}

#ifdef _WIN32

#ifndef SIO_AF_UNIX_GETPEERPID
#define SIO_AF_UNIX_GETPEERPID _WSAIOR(IOC_VENDOR, 256)
#endif

namespace {

// The user of a process token, as a copy of the SID, because the token buffer lives only inside
// this function. Empty when the token cannot be read.
std::vector<unsigned char> user_of(HANDLE process) {
    HANDLE token = nullptr;

    if (OpenProcessToken(process, TOKEN_QUERY, &token) == 0) return {};

    DWORD much = 0;

    (void)GetTokenInformation(token, TokenUser, nullptr, 0, &much);

    std::vector<unsigned char> room(much);

    std::vector<unsigned char> made;

    if (much > 0 && GetTokenInformation(token, TokenUser, room.data(), much, &much) != 0) {
        const auto* told = reinterpret_cast<const TOKEN_USER*>(room.data());
        const DWORD length = GetLengthSid(told->User.Sid);

        made.assign(static_cast<const unsigned char*>(told->User.Sid),
                    static_cast<const unsigned char*>(told->User.Sid) + length);
    }

    CloseHandle(token);

    return made;
}

}

// Whether the other side of a local socket is the same user. The socket lives in a directory no one
// else may access (`fs::runtime`), but a daemon asked about shares should not rely on a single
// barrier, so it asks the system who is really connecting. Windows gives the process number, and
// the user is taken from its token. Empty means the system cannot tell (not a local socket, an old
// system); the caller decides whether that is a refusal.
std::optional<bool> kindred(Handle one) {
    ULONG pid = 0;
    DWORD got = 0;

    if (pipes::piped(one)) {
        const std::optional<unsigned long> facing = pipes::facing(one);

        if (!facing) return std::nullopt;

        pid = static_cast<ULONG>(*facing);
    } else if (WSAIoctl(static_cast<SOCKET>(one), SIO_AF_UNIX_GETPEERPID, nullptr, 0, &pid, sizeof pid, &got, nullptr,
                        nullptr) != 0) {
        return std::nullopt;
    }

    // A process we may not even inspect belongs to someone else.
    HANDLE theirs = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);

    if (theirs == nullptr) return false;

    const std::vector<unsigned char> them = user_of(theirs);
    const std::vector<unsigned char> us = user_of(GetCurrentProcess());

    CloseHandle(theirs);

    return !them.empty() && them == us;
}

#else

// Whether the other side of a local socket is the same user. Linux (SO_PEERCRED) and the BSDs
// (getpeereid) say so directly; elsewhere the answer is empty and the caller decides whether that
// is a refusal.
std::optional<bool> kindred(Handle one) {
#if defined(SO_PEERCRED)
    ucred told{};
    socklen_t much = sizeof told;

    if (::getsockopt(one, SOL_SOCKET, SO_PEERCRED, &told, &much) != 0) return std::nullopt;

    return told.uid == ::geteuid();
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
    uid_t uid = 0;
    gid_t gid = 0;

    if (::getpeereid(one, &uid, &gid) != 0) return std::nullopt;

    return uid == ::geteuid();
#else
    (void)one;

    return std::nullopt;
#endif
}

#endif

namespace {

// A local socket leaves a file behind, and binding to an existing one is refused, so a program that
// crashed leaves a file that keeps the next one from starting.
//
// The file is removed only when nobody listens on it, and the proof of that is a refused
// connection. A successful connection means someone is already there, and then we must step aside
// rather than remove someone else's socket. Any other answer from the system is ambiguous, so the
// file is left alone then too: the bind will fail in a moment by itself and tell the caller.
void reaping(const Address& where) {
    sockaddr_storage told{};

    const int much = filled(where, told);

    const Handle probe = opened(Family::local, true);

    if (probe == nothing) return;

    const int made = ::connect(static_cast<int>(probe), reinterpret_cast<sockaddr*>(&told), much);
    const int why = made == 0 ? 0 : amiss();

    ending(probe);

    if (made == 0) return;

#ifdef _WIN32
    const bool dead = why == WSAECONNREFUSED;
#else
    const bool dead = why == ECONNREFUSED;
#endif

    if (dead) (void)std::remove(where.path().c_str());
}

}

// Binds the socket to the address and starts listening with a backlog of `waiting`. For a local
// socket a stale file is reaped first and the file is restricted to its owner afterwards.
bool listening(Handle one, const Address& where, int waiting) {
    sockaddr_storage told{};

    const int much = filled(where, told);

    if (where.family() == Family::local) reaping(where);

    if (::bind(static_cast<int>(one), reinterpret_cast<sockaddr*>(&told), much) != 0)
        return false;

    // Narrowing, not a barrier. The socket file is created with permissions taken from the process
    // umask, so usually accessible to everyone on the machine, and a socket any user can reach is a
    // hole. Permissions of others are therefore removed right after binding.
    //
    // The gap between the bind and this change cannot be closed from inside, and we do not pretend
    // it can: the real protection is the directory the socket lives in. Whoever keeps it in a
    // directory accessible only to themselves has it closed from the first moment.
#ifndef _WIN32
    if (where.family() == Family::local) (void)::chmod(where.path().c_str(), S_IRUSR | S_IWUSR);
#endif

    return ::listen(static_cast<int>(one), waiting) == 0;
}

// Binds a socket to an address without listening (for datagram sockets).
bool bound(Handle one, const Address& where) {
    sockaddr_storage told{};

    const int much = filled(where, told);

    return ::bind(static_cast<int>(one), reinterpret_cast<sockaddr*>(&told), much) == 0;
}

// A datagram larger than the buffer is silently truncated, which is one of the nastiest traps of
// UDP. It can be detected: Linux with MSG_TRUNC returns the real length, and Windows refuses
// outright with WSAEMSGSIZE. The truncation is reported through `cut` instead of handing over a
// piece that pretends to be the whole.
long fetching(Handle one, void* into, std::size_t much, Address& from, bool& cut) {
    sockaddr_storage told{};

    cut = false;

#ifdef _WIN32
    int wide = sizeof told;

    const long made = static_cast<long>(::recvfrom(static_cast<SOCKET>(one),
                                                   static_cast<char*>(into),
                                                   static_cast<int>(much), 0,
                                                   reinterpret_cast<sockaddr*>(&told), &wide));

    if (made < 0 && amiss() == WSAEMSGSIZE) {
        cut = true;

        from = seen(told);

        return static_cast<long>(much);
    }
#else
    socklen_t wide = sizeof told;

    int how = 0;

#ifdef MSG_TRUNC
    how = MSG_TRUNC;
#endif

    long made = static_cast<long>(::recvfrom(one, into, much, how,
                                             reinterpret_cast<sockaddr*>(&told), &wide));

    if (made >= 0 && static_cast<std::size_t>(made) > much) {
        cut = true;
        made = static_cast<long>(much);
    }
#endif

    if (made >= 0) from = seen(told);

    return made;
}

// A datagram goes out whole or not at all; unlike a stream, there is nothing to finish in a loop.
long sending(Handle one, const void* from, std::size_t much, const Address& to) {
    sockaddr_storage told{};

    const int wide = filled(to, told);

#ifdef _WIN32
    const int room = much > static_cast<std::size_t>(brimful) ? brimful : static_cast<int>(much);

    return static_cast<long>(::sendto(static_cast<SOCKET>(one), static_cast<const char*>(from),
                                      room, 0, reinterpret_cast<sockaddr*>(&told), wide));
#else
    return static_cast<long>(
        ::sendto(one, from, much, 0, reinterpret_cast<sockaddr*>(&told), wide));
#endif
}

// Windows takes the length as int, Unix systems as size_t. Narrowing on every system would give
// zero on Unix for every length that is a multiple of the four-gigabyte boundary, and a zero write
// would not advance the counter, so the step loop would spin forever on a full core, while a zero
// read would look like the end of the stream. Where narrowing is needed, the length is clamped
// instead of wrapped: a short write is finished by the next request anyway.
long getting(Handle one, void* into, std::size_t much) {
#ifdef _WIN32
    const int wide = much > static_cast<std::size_t>(brimful) ? brimful : static_cast<int>(much);

    return static_cast<long>(::recv(static_cast<SOCKET>(one), static_cast<char*>(into), wide, 0));
#else
    return static_cast<long>(::recv(one, into, much, 0));
#endif
}

// Writes up to `much` bytes to a stream socket; returns the count written or a negative value on
// error. The length is clamped on Windows like in `getting`.
long putting(Handle one, const void* from, std::size_t much) {
#ifdef _WIN32
    const int wide = much > static_cast<std::size_t>(brimful) ? brimful : static_cast<int>(much);

    return static_cast<long>(
        ::send(static_cast<SOCKET>(one), static_cast<const char*>(from), wide, 0));
#else
    // See `opened`: without this flag, writing to a socket after disconnection kills the caller's
    // process.
#ifdef MSG_NOSIGNAL
    return static_cast<long>(::send(one, from, much, MSG_NOSIGNAL));
#else
    return static_cast<long>(::send(one, from, much, 0));
#endif
#endif
}

// "Not yet" is not an error but an invitation to wait again. Windows uses a different number for it
// than Unix systems, so the question lives here rather than in five places.
bool again() {
#ifdef _WIN32
    return amiss() == WSAEWOULDBLOCK;
#else
    return amiss() == EAGAIN || amiss() == EWOULDBLOCK || amiss() == EINTR;
#endif
}

// A non-blocking connect almost never completes at once; it says "started", which is not an error.
// The number of that answer differs on every system, so the question lives in one place.
bool underway() {
#ifdef _WIN32
    return amiss() == WSAEWOULDBLOCK || amiss() == WSAEINPROGRESS;
#else
    return amiss() == EINPROGRESS || amiss() == EINTR;
#endif
}

// Shuts both directions of a socket. A named pipe is not half-closed: DisconnectNamedPipe throws
// away what the other side has not read yet. The other side sees the end when the handle is closed,
// once the last request is gone.
void shutting(Handle one) {
    if (one == nothing || pipes::piped(one)) return;

#ifdef _WIN32
    ::shutdown(static_cast<SOCKET>(one), SD_BOTH);
#else
    ::shutdown(one, SHUT_RDWR);
#endif
}

// Accepts one connection: `made` receives the new non-blocking socket (or `nothing`) and the result
// is the peer's address.
Address taken(Handle one, Handle& made) {
    sockaddr_storage told{};

#ifdef _WIN32
    int much = sizeof told;

    made = ::accept(static_cast<SOCKET>(one), reinterpret_cast<sockaddr*>(&told), &much);

    if (made == nothing) return {};

    u_long yes = 1;

    ::ioctlsocket(static_cast<SOCKET>(made), FIONBIO, &yes);
#else
    socklen_t much = sizeof told;

    // The accepted socket must not reach a child program either.
    made = sealed::accepted(one, reinterpret_cast<sockaddr*>(&told), &much);

    if (made < 0) {
        made = nothing;

        return {};
    }
#endif

    return seen(told);
}

}
