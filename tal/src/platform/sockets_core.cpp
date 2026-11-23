#include "../netting.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>

#include <ws2tcpip.h>

#include <afunix.h>

#include <mutex>
#else
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "sealed.hpp"
#endif

// The system side of the network, ported from talcite's inside.cpp: sockets opened non-blocking and
// sealed from child programs, the conversion between `Address` and the system's structures, the
// calls of sockets one by one, and what their codes of failure mean.
namespace tal::net::netting {

namespace {

#ifdef _WIN32
// The largest byte count passed in one call, since Windows takes the length as an int.
constexpr int brimful = 1 << 30;
#endif

// Fills `into` with the system's form of `from`; gives its length, or 0 for an empty address. An
// empty address is no IPv6: a default `Address` must not become [::]:0, on which a bind would
// succeed instead of being refused.
int filled(const Address& from, sockaddr_storage& into) {
    std::memset(&into, 0, sizeof into);

    if (from.family() == Family::four) {
        auto* four = reinterpret_cast<sockaddr_in*>(&into);

        four->sin_family = AF_INET;
        four->sin_port = htons(from.port());

        std::memcpy(&four->sin_addr, from.bytes().data(), 4);

        return static_cast<int>(sizeof(sockaddr_in));
    }

    if (from.family() == Family::local) {
        auto* nearby = reinterpret_cast<sockaddr_un*>(&into);
        const std::string& said = from.path();

        nearby->sun_family = AF_UNIX;

        std::memcpy(nearby->sun_path, said.data(), said.size());

        return static_cast<int>(offsetof(sockaddr_un, sun_path) + said.size() + 1);
    }

    if (from.family() != Family::six) return 0;

    auto* six = reinterpret_cast<sockaddr_in6*>(&into);

    six->sin6_family = AF_INET6;
    six->sin6_port = htons(from.port());
    six->sin6_scope_id = from.scope();

    std::memcpy(&six->sin6_addr, from.bytes().data(), 16);

    return static_cast<int>(sizeof(sockaddr_in6));
}

// The address of the system's structure `from`; empty for an unknown family, and for a local
// socket without a name, as our end of a local connection is, or with one `Address` refuses. A
// local path is read with the separators of `Path`.
Address seen(const sockaddr_storage& from) {
    std::array<std::byte, 16> raw{};

    if (from.ss_family == AF_INET) {
        const auto* four = reinterpret_cast<const sockaddr_in*>(&from);

        std::memcpy(raw.data(), &four->sin_addr, 4);

        return Address{Family::four, raw, ntohs(four->sin_port)};
    }

    if (from.ss_family == AF_UNIX) {
        const auto* nearby = reinterpret_cast<const sockaddr_un*>(&from);
        std::size_t much = 0;

        while (much < sizeof nearby->sun_path && nearby->sun_path[much] != '\0') ++much;

        std::string said{nearby->sun_path, much};

#ifdef _WIN32
        std::replace(said.begin(), said.end(), '\\', '/');
#endif

        try {
            return Address::local(Path::restored(said));
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

#ifdef _WIN32

namespace {

std::once_flag once;

// The UTF-8 text `said` as Windows's UTF-16.
std::wstring widened(const std::string& said) {
    const int much = MultiByteToWideChar(CP_UTF8, 0, said.data(), static_cast<int>(said.size()), nullptr, 0);
    std::wstring made(static_cast<std::size_t>(much), L'\0');

    (void)MultiByteToWideChar(CP_UTF8, 0, said.data(), static_cast<int>(said.size()), made.data(), much);

    return made;
}

}

// Starts Winsock once, before the first call of anything; it is never cleaned up, since the
// library does not know who else uses sockets, and the system cleans up when the process ends.
void starting() {
    std::call_once(once, [] {
        WSADATA data{};

        (void)WSAStartup(MAKEWORD(2, 2), &data);
    });
}

// The code of the last failed call of sockets on this thread.
int amiss() { return WSAGetLastError(); }

// The system's sentence for the code `code` in UTF-8, without its line end and period; empty for
// 0, the number when the system has none.
std::string spoken(int code) {
    if (code == 0) return {};

    wchar_t* said = nullptr;
    const DWORD much = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                                      static_cast<DWORD>(code), 0, reinterpret_cast<wchar_t*>(&said), 0, nullptr);

    if (much == 0 || said == nullptr) return std::to_string(code);

    const int wide = WideCharToMultiByte(CP_UTF8, 0, said, static_cast<int>(much), nullptr, 0, nullptr, nullptr);
    std::string made(static_cast<std::size_t>(wide), '\0');

    (void)WideCharToMultiByte(CP_UTF8, 0, said, static_cast<int>(much), made.data(), wide, nullptr, nullptr);
    LocalFree(said);

    while (!made.empty() && (made.back() == '\r' || made.back() == '\n' || made.back() == ' ' || made.back() == '.')) made.pop_back();

    return made;
}

// The kind of trouble the code `code` of Winsock means, or of Windows for a file that is not there.
::tal::Kind kind_of(int code) {
    switch (code) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND: return ::tal::Kind::missing;
        case WSAECONNREFUSED: return ::tal::Kind::refused;
        case WSAECONNRESET:
        case WSAECONNABORTED:
        case WSAESHUTDOWN:
        case WSAENOTCONN: return ::tal::Kind::broken;
        case WSAEADDRINUSE: return ::tal::Kind::busy;
        case WSAEACCES: return ::tal::Kind::denied;
        case WSAEHOSTUNREACH:
        case WSAENETUNREACH:
        case WSAEADDRNOTAVAIL:
        case WSAHOST_NOT_FOUND:
        case WSANO_DATA: return ::tal::Kind::missing;
        case WSAETIMEDOUT: return ::tal::Kind::overdue;
        case WSAEAFNOSUPPORT:
        case WSAEPROTONOSUPPORT: return ::tal::Kind::unsupported;
        case WSAENOBUFS:
        case WSAEMFILE: return ::tal::Kind::full;
        default: return ::tal::Kind::other;
    }
}

#else

// Nothing to start outside Windows.
void starting() {}

// The code of the last failed call of sockets (`errno`).
int amiss() { return errno; }

// The system's sentence for the code `code`; empty for 0.
std::string spoken(int code) {
    if (code == 0) return {};

    return std::strerror(code);
}

// The kind of trouble the code `code` of `errno` means.
::tal::Kind kind_of(int code) {
    switch (code) {
        case ECONNREFUSED: return ::tal::Kind::refused;
        case ECONNRESET:
        case ECONNABORTED:
        case EPIPE:
        case ENOTCONN: return ::tal::Kind::broken;
        case EADDRINUSE: return ::tal::Kind::busy;
        case EACCES:
        case EPERM: return ::tal::Kind::denied;
        case EHOSTUNREACH:
        case ENETUNREACH:
        case EADDRNOTAVAIL:
        case ENOENT: return ::tal::Kind::missing;
        case ETIMEDOUT: return ::tal::Kind::overdue;
        case EAFNOSUPPORT:
        case EPROTONOSUPPORT: return ::tal::Kind::unsupported;
        case ENOBUFS:
        case EMFILE:
        case ENFILE:
        case ENOSPC: return ::tal::Kind::full;
        default: return ::tal::Kind::other;
    }
}

#endif

// The code a request gets when its socket is closed under it.
int aborted() {
#ifdef _WIN32
    return WSAECONNABORTED;
#else
    return ECONNABORTED;
#endif
}

// Throws `Trouble` saying `said` and the system's sentence for `code`, of the kind the code means,
// at the address `where`.
void failed(const std::string& said, int code, const Address& where) {
    const std::string sentence = spoken(code);

    throw Trouble{kind_of(code), said + (sentence.empty() ? "." : ": " + sentence + "."), where.spelled(), code};
}

// The text of `where` a local socket's structure takes: the bytes of the path outside Windows, its
// UTF-8 on Windows, with the system's separators.
std::string pathed(const Path& where) {
#ifdef _WIN32
    std::string made = where.text();

    std::replace(made.begin(), made.end(), '/', '\\');

    return made;
#else
    return std::string{where.stored()};
#endif
}

// Fills the system's structure at `into` with `from`; gives its length.
int filling(const Address& from, void* into) { return filled(from, *static_cast<sockaddr_storage*>(into)); }

// The address of the system's structure at `from`.
Address facing(const void* from) { return seen(*static_cast<const sockaddr_storage*>(from)); }

// A socket of `family`, of a stream or of datagrams, non-blocking and sealed from child programs;
// `nothing` on failure. A local socket is of a stream only, everywhere. On IPv6 it takes IPv4 too,
// and writing to a socket the other side closed never kills the program with SIGPIPE.
Handle opened(Family family, bool stream) {
    starting();

    if (family == Family::local && !stream) return nothing;

    const int which = family == Family::local ? AF_UNIX : family == Family::four ? AF_INET : AF_INET6;

#ifdef _WIN32
    const Handle made = ::socket(which, stream ? SOCK_STREAM : SOCK_DGRAM, 0);

    if (made == nothing) return nothing;

    u_long yes = 1;

    (void)::ioctlsocket(static_cast<SOCKET>(made), FIONBIO, &yes);
    (void)::SetHandleInformation(reinterpret_cast<HANDLE>(made), HANDLE_FLAG_INHERIT, 0);

    if (!stream) {
        BOOL off = FALSE;
        DWORD given = 0;

        (void)::WSAIoctl(static_cast<SOCKET>(made), 0x9800000CUL, &off, sizeof off, nullptr, 0, &given, nullptr, nullptr);
    }
#else
    const Handle made = sealed::socketed(which, stream ? SOCK_STREAM : SOCK_DGRAM);

    if (made < 0) return nothing;

#ifdef SO_NOSIGPIPE
    {
        const int yes = 1;

        (void)::setsockopt(made, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
    }
#endif
#endif

    if (family == Family::six) {
        const int off = 0;

        (void)::setsockopt(made, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof off);
    }

    return made;
}

// Closes the socket `one`; `nothing` is let be.
void ending(Handle one) {
    if (one == nothing) return;

#ifdef _WIN32
    (void)::closesocket(static_cast<SOCKET>(one));
#else
    (void)::close(one);
#endif
}

// Starts connecting `one` to `where`; true only when it connected at once, as on the loopback.
bool joined(Handle one, const Address& where) {
    sockaddr_storage told{};
    const int much = filled(where, told);

    return ::connect(one, reinterpret_cast<sockaddr*>(&told), much) == 0;
}

// How a connect on `one` ended: 0 when it connected, otherwise the code why not. Readiness for
// writing says only that it ended.
int settling(Handle one) {
    int why = 0;
#ifdef _WIN32
    int much = sizeof why;
#else
    socklen_t much = sizeof why;
#endif

    if (::getsockopt(one, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&why), &much) != 0) return amiss();

    return why;
}

// The address of our end of `one`, or of the other end (`peer`); empty when the system cannot say.
Address named(Handle one, bool peer) {
    sockaddr_storage told{};
#ifdef _WIN32
    int much = sizeof told;
#else
    socklen_t much = sizeof told;
#endif

    const int made = peer ? ::getpeername(one, reinterpret_cast<sockaddr*>(&told), &much) : ::getsockname(one, reinterpret_cast<sockaddr*>(&told), &much);

    if (made != 0) return {};

    return seen(told);
}

// Lets a server bind its port again at once after a close, past TIME_WAIT; not on Windows, where
// the option means taking over another's port.
void sharing(Handle one) {
#ifndef _WIN32
    const int yes = 1;

    (void)::setsockopt(one, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
#else
    (void)one;
#endif
}

// Lets `one` send to the broadcast address, or not (`given`).
void broadcasting(Handle one, bool given) {
    const int yes = given ? 1 : 0;

    (void)::setsockopt(one, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof yes);
}

#ifdef _WIN32

#ifndef SIO_AF_UNIX_GETPEERPID
#define SIO_AF_UNIX_GETPEERPID _WSAIOR(IOC_VENDOR, 256)
#endif

namespace {

// The user of the token of `process`, as a copy of its SID; empty when the token cannot be read.
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

        made.assign(static_cast<const unsigned char*>(told->User.Sid), static_cast<const unsigned char*>(told->User.Sid) + length);
    }

    CloseHandle(token);

    return made;
}

}

// Whether the other side of the local socket `one` runs as our user: the system gives its process,
// and the user comes from its token. Nothing when the system cannot say; the caller decides
// whether that refuses.
std::optional<bool> kindred(Handle one) {
    ULONG pid = 0;
    DWORD got = 0;

    if (WSAIoctl(static_cast<SOCKET>(one), SIO_AF_UNIX_GETPEERPID, nullptr, 0, &pid, sizeof pid, &got, nullptr, nullptr) != 0) return std::nullopt;

    HANDLE theirs = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);

    if (theirs == nullptr) return false;

    const std::vector<unsigned char> them = user_of(theirs);
    const std::vector<unsigned char> us = user_of(GetCurrentProcess());

    CloseHandle(theirs);

    return !them.empty() && them == us;
}

#else

// Whether the other side of the local socket `one` runs as our user, as Linux (SO_PEERCRED) and the
// BSDs (getpeereid) say; nothing elsewhere, and the caller decides whether that refuses.
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

#ifdef _WIN32

// The code a refused connect to the local socket at `where` means: Windows refuses alike when the
// file is not there, and that is a missing file, not a refusal.
int refusal(const Address& where) {
    const DWORD found = GetFileAttributesW(widened(where.path()).c_str());

    if (found == INVALID_FILE_ATTRIBUTES && (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND))
        return ERROR_FILE_NOT_FOUND;

    return WSAECONNREFUSED;
}

#endif

// Removes the file of the local socket at `where`; a file already gone is let be.
void removed(const Address& where) {
    if (where.family() != Family::local) return;

#ifdef _WIN32
    (void)DeleteFileW(widened(where.path()).c_str());
#else
    (void)::unlink(where.path().c_str());
#endif
}

namespace {

// Removes the file of a local socket at `where` left by a program that is gone: only when a connect
// to it is refused, since a connect that succeeds means someone listens there, and any other answer
// leaves the file for the bind to refuse.
void reaped(const Address& where) {
    sockaddr_storage told{};
    const int much = filled(where, told);
    const Handle probe = opened(Family::local, true);

    if (probe == nothing) return;

    const int made = ::connect(probe, reinterpret_cast<sockaddr*>(&told), much);
    const int why = made == 0 ? 0 : amiss();

    ending(probe);

    if (made == 0) return;

#ifdef _WIN32
    const bool dead = why == WSAECONNREFUSED;
#else
    const bool dead = why == ECONNREFUSED;
#endif

    if (dead) removed(where);
}

}

// Binds `one` to `where` and listens with a backlog of `waiting`. A local socket's stale file is
// reaped first, and its new file is for its owner alone right after the bind; the real barrier is
// the folder it stands in.
bool listening(Handle one, const Address& where, int waiting) {
    sockaddr_storage told{};
    const int much = filled(where, told);

    if (where.family() == Family::local) reaped(where);

    if (::bind(one, reinterpret_cast<sockaddr*>(&told), much) != 0) return false;

#ifndef _WIN32
    if (where.family() == Family::local) (void)::chmod(where.path().c_str(), S_IRUSR | S_IWUSR);
#endif

    return ::listen(one, waiting) == 0;
}

// Binds `one` to `where` without listening, as a socket of datagrams is.
bool bound(Handle one, const Address& where) {
    sockaddr_storage told{};
    const int much = filled(where, told);

    return ::bind(one, reinterpret_cast<sockaddr*>(&told), much) == 0;
}

// Takes one datagram from `one` into `much` bytes at `into`, its sender into `from`; gives its
// length, or a negative value on failure. A datagram larger than the room is cut, and `cut` says
// so instead of handing a piece that pretends to be the whole.
long fetching(Handle one, void* into, std::size_t much, Address& from, bool& cut) {
    sockaddr_storage told{};

    cut = false;

#ifdef _WIN32
    int wide = sizeof told;
    const int room = much > static_cast<std::size_t>(brimful) ? brimful : static_cast<int>(much);
    const long made = static_cast<long>(::recvfrom(static_cast<SOCKET>(one), static_cast<char*>(into), room, 0, reinterpret_cast<sockaddr*>(&told), &wide));

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

    long made = static_cast<long>(::recvfrom(one, into, much, how, reinterpret_cast<sockaddr*>(&told), &wide));

    if (made >= 0 && static_cast<std::size_t>(made) > much) {
        cut = true;
        made = static_cast<long>(much);
    }
#endif

    if (made >= 0) from = seen(told);

    return made;
}

// Sends `much` bytes at `from` to `to` as one datagram; gives the count sent, or a negative value.
long sending(Handle one, const void* from, std::size_t much, const Address& to) {
    sockaddr_storage told{};
    const int wide = filled(to, told);

#ifdef _WIN32
    const int room = much > static_cast<std::size_t>(brimful) ? brimful : static_cast<int>(much);

    return static_cast<long>(::sendto(static_cast<SOCKET>(one), static_cast<const char*>(from), room, 0, reinterpret_cast<sockaddr*>(&told), wide));
#else
    return static_cast<long>(::sendto(one, from, much, 0, reinterpret_cast<sockaddr*>(&told), wide));
#endif
}

// Reads at most `much` bytes from `one` into `into`; gives the count, 0 at the end, or a negative
// value. Windows takes the length as an int, so there it is clamped, not wrapped.
long getting(Handle one, void* into, std::size_t much) {
#ifdef _WIN32
    const int wide = much > static_cast<std::size_t>(brimful) ? brimful : static_cast<int>(much);

    return static_cast<long>(::recv(static_cast<SOCKET>(one), static_cast<char*>(into), wide, 0));
#else
    return static_cast<long>(::recv(one, into, much, 0));
#endif
}

// Writes at most `much` bytes at `from` to `one`; gives the count, or a negative value. A peer gone
// gives an error, not SIGPIPE.
long putting(Handle one, const void* from, std::size_t much) {
#ifdef _WIN32
    const int wide = much > static_cast<std::size_t>(brimful) ? brimful : static_cast<int>(much);

    return static_cast<long>(::send(static_cast<SOCKET>(one), static_cast<const char*>(from), wide, 0));
#elif defined(MSG_NOSIGNAL)
    return static_cast<long>(::send(one, from, much, MSG_NOSIGNAL));
#else
    return static_cast<long>(::send(one, from, much, 0));
#endif
}

// Whether the last failure only said "not yet".
bool again() {
#ifdef _WIN32
    return amiss() == WSAEWOULDBLOCK;
#else
    return amiss() == EAGAIN || amiss() == EWOULDBLOCK || amiss() == EINTR;
#endif
}

// Whether the last failed connect only said "started".
bool underway() {
#ifdef _WIN32
    return amiss() == WSAEWOULDBLOCK || amiss() == WSAEINPROGRESS;
#else
    return amiss() == EINPROGRESS || amiss() == EINTR;
#endif
}

// Shuts both directions of `one`, which does not give its number back to the system.
void shutting(Handle one) {
    if (one == nothing) return;

#ifdef _WIN32
    (void)::shutdown(static_cast<SOCKET>(one), SD_BOTH);
#else
    (void)::shutdown(one, SHUT_RDWR);
#endif
}

// Accepts one connection on `one` into `made`, non-blocking and sealed, or `nothing`; gives the
// address of its other end.
Address taken(Handle one, Handle& made) {
    sockaddr_storage told{};

#ifdef _WIN32
    int much = sizeof told;

    made = ::accept(static_cast<SOCKET>(one), reinterpret_cast<sockaddr*>(&told), &much);

    if (made == nothing) return {};

    u_long yes = 1;

    (void)::ioctlsocket(static_cast<SOCKET>(made), FIONBIO, &yes);
    (void)::SetHandleInformation(reinterpret_cast<HANDLE>(made), HANDLE_FLAG_INHERIT, 0);
#else
    socklen_t much = sizeof told;

    made = sealed::accepted(one, reinterpret_cast<sockaddr*>(&told), &much);

    if (made < 0) {
        made = nothing;

        return {};
    }
#endif

    return seen(told);
}

namespace {

// How many bytes the path of a local socket may hold: the shortest `sun_path` of the systems
// (104 on macOS, 108 elsewhere) without its terminating zero. Refused when the address is made,
// with the path in the sentence, not at the bind as a bare code.
constexpr std::size_t longest = 103;

// The interface after `%` in a link-local address, as a number (`%3`, as Windows writes it) or a
// name (`%eth0`, as Unix systems do); 0 for a name nobody knows.
std::uint32_t marked(const std::string& said) {
    if (said.empty()) return 0;

    if (said.size() <= 9 && std::all_of(said.begin(), said.end(), [](char one) { return one >= '0' && one <= '9'; }))
        return static_cast<std::uint32_t>(std::stoul(said));

#ifdef _WIN32
    return 0;
#else
    return ::if_nametoindex(said.c_str());
#endif
}

// The sentence for a code of getaddrinfo, which on Unix systems has its own table.
std::string looked_amiss(int code) {
#ifdef _WIN32
    return spoken(code);
#else
    return ::gai_strerror(code);
#endif
}

// The kind of trouble a code of getaddrinfo means: a name nobody knows is missing, and a server
// of names that failed for now, to be asked again later, is busy.
::tal::Kind looked_kind(int code) {
#ifdef _WIN32
    if (code == WSATRY_AGAIN) return ::tal::Kind::busy;

    return kind_of(code);
#else
    switch (code) {
        case EAI_AGAIN: return ::tal::Kind::busy;
        case EAI_NONAME:
#ifdef EAI_NODATA
        case EAI_NODATA:
#endif
        case EAI_FAIL: return ::tal::Kind::missing;
        case EAI_FAMILY: return ::tal::Kind::unsupported;
        case EAI_MEMORY: return ::tal::Kind::full;
        default: return ::tal::Kind::other;
    }
#endif
}

}

// The addresses of the name `host`, each with the port `port`, of the family `family` (any for
// none), in the system's order; blocks, so it runs on the io pool. Throws `Trouble`
// for a name nobody knows or one without addresses.
std::vector<Address> looked(const std::string& host, std::uint16_t port, Family family) {
    starting();

    if (family == Family::local) throw Trouble{::tal::Kind::unsupported, "A name has no local address.", host};

    addrinfo wanted{};

    wanted.ai_family = family == Family::four ? AF_INET : family == Family::six ? AF_INET6 : AF_UNSPEC;
    wanted.ai_socktype = SOCK_STREAM;

    addrinfo* found = nullptr;
    const int code = ::getaddrinfo(host.c_str(), nullptr, &wanted, &found);

    if (code != 0) throw Trouble{looked_kind(code), "Cannot find the name: " + looked_amiss(code) + ".", host, code};

    std::vector<Address> made;

    try {
        for (const addrinfo* one = found; one != nullptr; one = one->ai_next) {
            if (one->ai_addr == nullptr || (one->ai_family != AF_INET && one->ai_family != AF_INET6)) continue;

            sockaddr_storage told{};

            std::memcpy(&told, one->ai_addr, std::min(sizeof told, static_cast<std::size_t>(one->ai_addrlen)));

            Address which = seen(told);

            which.port(port);
            made.push_back(std::move(which));
        }
    } catch (...) {
        ::freeaddrinfo(found);

        throw;
    }

    ::freeaddrinfo(found);

    if (made.empty()) throw Trouble{::tal::Kind::missing, "The name has no addresses.", host};

    return made;
}

}

namespace tal::net {

// An IPv4 or IPv6 address read from `text`, in brackets or not, with the interface after `%`.
// Throws `Trouble` for anything else, a name too: names are for `resolve`. A zero inside the text
// is refused, since the system would read only up to it.
Address::Address(std::string_view text, std::uint16_t port) {
    netting::starting();

    std::string said{text};

    if (said.find('\0') != std::string::npos) throw Trouble{Kind::malformed, "Not an address: the text has a zero inside."};

    if (said.size() >= 2 && said.front() == '[' && said.back() == ']') said = said.substr(1, said.size() - 2);

    std::uint32_t scope = 0;

    if (const std::size_t mark = said.find('%'); mark != std::string::npos) {
        const std::string tail = said.substr(mark + 1);

        scope = netting::marked(tail);

        if (scope == 0) throw Trouble{Kind::malformed, "Not an interface: " + tail + "."};

        said.resize(mark);
    }

    in_addr four{};

    if (scope == 0 && ::inet_pton(AF_INET, said.c_str(), &four) == 1) {
        std::memcpy(bytes_.data(), &four, 4);
        family_ = Family::four;
        port_ = port;

        return;
    }

    in6_addr six{};

    if (::inet_pton(AF_INET6, said.c_str(), &six) == 1) {
        std::memcpy(bytes_.data(), &six, 16);
        family_ = Family::six;
        scope_ = scope;
        port_ = port;

        return;
    }

    throw Trouble{Kind::malformed, "Not an address: " + std::string{text} + "."};
}

// An address of `family` made of its bytes, the first four for IPv4; a local address is made of a
// path (`local`), and an empty family makes an empty address.
Address::Address(Family family, const std::array<std::byte, 16>& bytes, std::uint16_t port) {
    if (family == Family::local) throw Trouble{Kind::other, "A local address is made of a path."};

    if (family == Family::none) return;

    bytes_ = bytes;
    port_ = port;
    family_ = family;

    if (family == Family::four) std::fill(bytes_.begin() + 4, bytes_.end(), std::byte{0});
}

// The address of a local socket at `where`. Throws for an empty path, a zero inside it, or one
// longer than `longest` bytes.
Address Address::local(const Path& where) {
    std::string said = netting::pathed(where);

    if (said.empty()) throw Trouble{Kind::other, "The path of a local socket is empty."};

    if (said.find('\0') != std::string::npos) throw Trouble{Kind::malformed, "The path of a local socket has a zero inside."};

    if (said.size() > netting::longest)
        throw Trouble{Kind::other, "The path of a local socket is longer than " + std::to_string(netting::longest) + " bytes.", said};

    Address made;

    made.family_ = Family::local;
    made.path_ = std::move(said);

    return made;
}

// Sets the port; a local address has none, and an empty one nothing to set it on.
void Address::port(std::uint16_t given) {
    if (family_ == Family::local || family_ == Family::none) throw Trouble{Kind::other, "This address has no port."};

    port_ = given;
}

// Sets the interface of an IPv6 address; other addresses have none.
void Address::scope(std::uint32_t given) {
    if (family_ != Family::six) throw Trouble{Kind::other, "Only an IPv6 address has an interface."};

    scope_ = given;
}

// The address of every interface of `family` (all zeros) with the port `port`, to bind to.
Address Address::any(Family family, std::uint16_t port) {
    if (family != Family::four && family != Family::six) throw Trouble{Kind::other, "Only IPv4 and IPv6 have an address of every interface."};

    Address made;

    made.family_ = family;
    made.port_ = port;

    return made;
}

// The loopback address of `family` (127.0.0.1 or ::1) with the port `port`.
Address Address::loopback(Family family, std::uint16_t port) {
    if (family != Family::four && family != Family::six) throw Trouble{Kind::other, "Only IPv4 and IPv6 have a loopback address."};

    Address made = any(family, port);

    if (family == Family::four) {
        made.bytes_[0] = std::byte{127};
        made.bytes_[3] = std::byte{1};
    } else {
        made.bytes_[15] = std::byte{1};
    }

    return made;
}

// The address alone, without the port: IPv4 dotted, IPv6 with `%` and its interface when it has
// one, the path of a local address; empty for an empty address.
std::string Address::text() const {
    if (family_ == Family::none) return {};

    if (family_ == Family::local) return path_;

    char made[INET6_ADDRSTRLEN]{};

    if (::inet_ntop(family_ == Family::four ? AF_INET : AF_INET6, bytes_.data(), made, sizeof made) == nullptr) return {};

    std::string said{made};

    if (family_ == Family::six && scope_ != 0) said += "%" + std::to_string(scope_);

    return said;
}

// The address with its port, to print and to read back: IPv6 in brackets, so that its colons are
// not taken for the port's; a local address is its path, which has no port.
std::string Address::spelled() const {
    if (family_ == Family::none || family_ == Family::local) return text();

    if (family_ == Family::four) return text() + ":" + std::to_string(port_);

    return "[" + text() + "]:" + std::to_string(port_);
}

// IPv4 as a socket of both families sees it (`::ffff:a.b.c.d`) as plain IPv4; any other address as
// it is.
Address Address::unmapped() const {
    if (family_ != Family::six) return *this;

    for (std::size_t at = 0; at < 10; ++at)
        if (bytes_[at] != std::byte{0}) return *this;

    if (bytes_[10] != std::byte{0xff} || bytes_[11] != std::byte{0xff}) return *this;

    Address made = any(Family::four, port_);

    std::copy(bytes_.begin() + 12, bytes_.end(), made.bytes_.begin());

    return made;
}

}
