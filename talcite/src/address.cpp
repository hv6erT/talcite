#include "talcite/address.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "talcite/trouble.hpp"
#include "starting.hpp"

#ifdef _WIN32
#include <winsock2.h>

#include <ws2tcpip.h>

#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace talcite::net {

namespace {

// How many bytes a local socket path can hold. The `sun_path` field has 108 bytes on Linux and on
// Windows but 104 on macOS, and one is taken by the terminating zero, so the limit is the shortest
// of them without that zero. Refusing when the address is built is better than refusing only when
// the socket is bound: there it comes out as a bare error number, with no hint of what was too
// long.
constexpr std::size_t longest = 103;

// The interface number after the percent sign in a link-local address. It is either a name
// (`%eth0`) or a number (`%3`): Windows writes numbers, Unix systems write names, and the same
// address has to be accepted on both. Returns 0 for an unknown name.
std::uint32_t marked(const std::string& said) {
    if (said.empty()) return 0;

    if (said.size() <= 9 && std::all_of(said.begin(), said.end(),
                                        [](char one) { return one >= '0' && one <= '9'; }))
        return static_cast<std::uint32_t>(std::stoul(said));

#ifdef _WIN32
    return 0;
#else
    return if_nametoindex(said.c_str());
#endif
}

// An address from the system's socket form. The same conversion is needed for getaddrinfo and for
// the list of interfaces, so it stands in one place. Returns false for a family other than IPv4 and
// IPv6.
bool drawn(const sockaddr* from, std::uint16_t port, Address& made) {
    std::array<std::byte, 16> raw{};

    if (from == nullptr) return false;

    if (from->sa_family == AF_INET) {
        const auto* four = reinterpret_cast<const sockaddr_in*>(from);

        std::memcpy(raw.data(), &four->sin_addr, 4);

        made = Address{Family::four, raw, port};

        return true;
    }

    if (from->sa_family == AF_INET6) {
        const auto* six = reinterpret_cast<const sockaddr_in6*>(from);

        std::memcpy(raw.data(), &six->sin6_addr, 16);

        made = Address{Family::six, raw, port};

        made.scope(six->sin6_scope_id);

        return true;
    }

    return false;
}

// Whether the address belongs to the asked family; `Family::none` accepts every family.
bool fitting(const Address& one, Family family) {
    return family == Family::none || one.family() == family;
}

// The numbers from getaddrinfo are neither errno values nor Winsock numbers: on Unix systems they
// have their own table. On Windows they are the same numbers as the rest of Winsock, so `spoken` is
// enough there.
std::string amiss_named(int code) {
#ifdef _WIN32
    return spoken(code);
#else
    return ::gai_strerror(code);
#endif
}

}

// An IPv4 or IPv6 address parsed from text, optionally in brackets and with an interface number
// after `%`. Throws `Trouble` for anything else, including a domain name.
Address::Address(std::string_view text, std::uint16_t port) {
    starting();

    std::string said{text};

    // A zero inside the text is not the end of the address. inet_pton takes a zero-terminated
    // string, so it would see only the prefix and nobody would check the rest: "1.2.3.4" glued to
    // garbage would build successfully, and whoever cuts a name out of a network buffer would get a
    // valid-looking address from a broken field.
    if (said.find('\0') != std::string::npos)
        throw Trouble{"Not an address: the text has a zero inside."};

    // The square brackets come off before the interface number, not after it. The other way round,
    // the closing bracket would fall into the number's text and `[fe80::1` would go to parsing, so
    // the form we print ourselves through `spelled()` could not be read back.
    if (said.size() >= 2 && said.front() == '[' && said.back() == ']')
        said = said.substr(1, said.size() - 2);

    // The interface number is cut off before parsing, because inet_pton does not know it.
    std::uint32_t scope = 0;

    if (const std::size_t mark = said.find('%'); mark != std::string::npos) {
        const std::string tail = said.substr(mark + 1);

        scope = marked(tail);

        // An unknown interface name must not fall silently to zero: a link-local address without a
        // number is useless, and the error would surface only at connection time, far from the
        // typo.
        if (scope == 0)
            throw Trouble{"Not an interface number: " + tail + "."};

        said.resize(mark);
    }

    in_addr four{};

    if (::inet_pton(AF_INET, said.c_str(), &four) == 1) {
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

    // A domain name is not an address, and we do not pretend it is; names are for `resolve`, which
    // can ask the world. A silent empty address would show up only as a failed connection.
    throw Trouble{"Not an address: " + std::string{text} + "."};
}

// A local address is not made from bytes or a port; `local()` is for that. Without this refusal one
// could build an address that calls itself local but carries sixteen zeros and a port number; it
// would pass `held()`, and only the socket would say that something is missing.
Address::Address(Family family, const std::array<std::byte, 16>& bytes, std::uint16_t port)
    : bytes_(bytes), port_(port), family_(family) {
    if (family == Family::local)
        throw Trouble{"A local address is not built from bytes — use local()."};
}

// A local socket address from a path. The path is taken by value and moved: the caller either
// builds it in place or gives up its own, and a copy inside would be a third instance of the same
// string. Throws for an empty path, a zero inside it, or more than `longest` bytes.
Address Address::local(std::string path) {
    if (path.empty()) throw Trouble{"The local socket path is empty."};

    // The same reason the text constructor refuses a zero inside: the path goes into a
    // zero-terminated field, so nobody would see the rest anyway.
    if (path.find('\0') != std::string::npos)
        throw Trouble{"The local socket path has a zero inside."};

    if (path.size() > longest)
        throw Trouble{"The local socket path is longer than " + std::to_string(longest) +
                      " bytes: " + path + "."};

    Address made;

    made.family_ = Family::local;
    made.path_ = std::move(path);

    return made;
}

// Sets the port. It refuses on a local address instead of storing a value nobody would ever read. A
// state that cannot be expressed is better than a state that cannot be noticed; it is the same
// principle `Key` stands on.
void Address::port(std::uint16_t given) {
    if (family_ == Family::local) throw Trouble{"A local address has no port."};

    port_ = given;
}

// Sets the interface number of an IPv6 address. It refuses on a local address, as `port` does.
void Address::scope(std::uint32_t given) {
    if (family_ == Family::local) throw Trouble{"A local address has no interface number."};

    scope_ = given;
}

// The wildcard address of the family (all zeros) with the given port, for binding to every
// interface. A local address has none.
Address Address::any(Family family, std::uint16_t port) {
    if (family == Family::local)
        throw Trouble{"A local address has no wildcard form — a path is required."};

    Address made;

    made.family_ = family;
    made.port_ = port;

    return made;
}

// The loopback address of the family (127.0.0.1 or ::1) with the given port.
Address Address::loopback(Family family, std::uint16_t port) {
    if (family == Family::local)
        throw Trouble{"A local socket is its own loopback — it has no separate address."};

    Address made = any(family, port);

    if (family == Family::four) {
        made.bytes_[0] = std::byte{127};
        made.bytes_[3] = std::byte{1};
    } else if (family == Family::six) {
        made.bytes_[15] = std::byte{1};
    }

    return made;
}

// The address alone, without the port: dotted IPv4, IPv6 with `%` and the interface number when
// there is one, or the path of a local address. Empty for an address that holds nothing.
std::string Address::text() const {
    if (!held()) return {};

    if (family_ == Family::local) return path_;

    char made[INET6_ADDRSTRLEN]{};

    const int which = family_ == Family::four ? AF_INET : AF_INET6;

    if (::inet_ntop(which, bytes_.data(), made, sizeof made) == nullptr) return {};

    std::string said{made};

    if (family_ == Family::six && scope_ != 0) said += "%" + std::to_string(scope_);

    return said;
}

// The address with its port, ready to print. An IPv6 address with a port is written in square
// brackets, because otherwise its colons could not be told apart. This is kept in one place so that
// it is not glued together in five.
std::string Address::spelled() const {
    if (!held()) return {};

    // The path without a port appended: a local socket has no port, and "/run/tw.sock:0" would be a
    // lie about what this address is.
    if (family_ == Family::local) return path_;

    if (family_ == Family::four) return text() + ":" + std::to_string(port_);

    return "[" + text() + "]:" + std::to_string(port_);
}

// IPv4 seen through a dual-stack socket (`::ffff:a.b.c.d`) is plain IPv4, and that is how it should
// be addressed and written. Any other address stays as it was.
Address Address::unmapped() const {
    if (family_ != Family::six) return *this;

    for (std::size_t at = 0; at < 10; ++at)
        if (bytes_[at] != std::byte{0}) return *this;

    if (bytes_[10] != std::byte{0xff} || bytes_[11] != std::byte{0xff}) return *this;

    Address made = any(Family::four, port_);

    for (std::size_t at = 0; at < 4; ++at) made.bytes_[at] = bytes_[12 + at];

    return made;
}

// The addresses of a host name, looked up with getaddrinfo on the pool, each with the given port
// and without duplicates. Fails with `Trouble` when the name is unknown or has no address of the
// asked family.
Promise<std::vector<Address>> resolve(std::string host, std::uint16_t port, Family family) {
    return Promise<std::vector<Address>>{[host, port, family](auto giving, auto amiss) {
        starting();

        addrinfo wanted{};

        wanted.ai_family = family == Family::four    ? AF_INET
                           : family == Family::six ? AF_INET6
                                                   : AF_UNSPEC;
        wanted.ai_socktype = SOCK_STREAM;

        addrinfo* found = nullptr;

        const int code = ::getaddrinfo(host.c_str(), nullptr, &wanted, &found);

        if (code != 0) {
            amiss(std::make_exception_ptr(
                Trouble{"Unknown name " + host + ": " + amiss_named(code) + ".", code}));

            return;
        }

        // A guard frees the system's list, not a line at the end: collecting can throw when memory
        // runs out, and the system allocation would then leak.
        struct Freeing {
            addrinfo* one;

            // Frees the list from getaddrinfo, however the lambda is left.
            ~Freeing() { ::freeaddrinfo(one); }
        } const freeing{found};

        std::vector<Address> made;

        for (const addrinfo* one = found; one != nullptr; one = one->ai_next) {
            Address which;

            if (!drawn(one->ai_addr, port, which)) continue;

            // The same address can come several times, once for every socket type.
            if (std::find(made.begin(), made.end(), which) == made.end()) made.push_back(which);
        }

        if (made.empty()) {
            amiss(std::make_exception_ptr(Trouble{"The name has no addresses: " + host + "."}));

            return;
        }

        giving(std::move(made));
    }};
}


// The list of this machine's own addresses. It is needed when gathering candidates for NAT
// punching: two machines in one local network should talk through the switch next to them, not
// through half of the internet.
//
// We return everything the system reports on interfaces that are up, loopback included. This is the
// question "what addresses do I have", not "which of them are good for something"; filtering
// belongs to the caller, because every use filters differently. Candidate gathering drops the
// loopback, so that the other side does not try to reach itself.
Promise<std::vector<Address>> mine(Family family) {
    return Promise<std::vector<Address>>{[family](auto giving, auto amiss) {
        starting();

        std::vector<Address> made;

#ifdef _WIN32
        // The buffer size can change between the question and the answer, because interfaces can
        // appear in the meantime. Hence a loop, not a single call with the size read back.
        ULONG much = 16384;

        std::vector<std::byte> room;

        IP_ADAPTER_ADDRESSES* first = nullptr;

        ULONG code = ERROR_BUFFER_OVERFLOW;

        for (int turn = 0; turn < 4 && code == ERROR_BUFFER_OVERFLOW; ++turn) {
            room.assign(much, std::byte{});

            first = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(room.data());

            code = ::GetAdaptersAddresses(AF_UNSPEC,
                                          GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                              GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME,
                                          nullptr, first, &much);
        }

        // ERROR_NO_DATA means "no addresses of this family", not a failure: POSIX returns an empty
        // list then, and Windows has to do the same.
        if (code != NO_ERROR && code != ERROR_NO_DATA) {
            amiss(std::make_exception_ptr(
                Trouble{"Cannot list own addresses: " + spoken(static_cast<int>(code)) + ".",
                        static_cast<int>(code)}));

            return;
        }

        for (const IP_ADAPTER_ADDRESSES* card = first; card != nullptr; card = card->Next) {
            if (card->OperStatus != IfOperStatusUp) continue;

            for (const IP_ADAPTER_UNICAST_ADDRESS* one = card->FirstUnicastAddress; one != nullptr;
                 one = one->Next) {
                Address which;

                if (!drawn(one->Address.lpSockaddr, 0, which)) continue;

                if (!fitting(which, family)) continue;

                if (std::find(made.begin(), made.end(), which) == made.end()) made.push_back(which);
            }
        }
#else
        ifaddrs* first = nullptr;

        if (::getifaddrs(&first) != 0) {
            amiss(std::make_exception_ptr(
                Trouble{"Cannot list own addresses: " + spoken(errno) + ".", errno}));

            return;
        }

        struct Freeing {
            ifaddrs* one;

            // Frees the list from getifaddrs, however the lambda is left.
            ~Freeing() { ::freeifaddrs(one); }
        } const freeing{first};

        for (const ifaddrs* one = first; one != nullptr; one = one->ifa_next) {
            if ((one->ifa_flags & IFF_UP) == 0) continue;

            Address which;

            if (!drawn(one->ifa_addr, 0, which)) continue;

            if (!fitting(which, family)) continue;

            // The interface number goes only on link-local addresses (fe80::/10). For the rest zero
            // is the normal state (glibc fills this field only for link addresses), and attaching
            // it to all of them would turn the loopback into `::1%1`, which would no longer equal
            // `::1` and would go out as a punching candidate.
            if (which.family() == Family::six && which.scope() == 0 && one->ifa_name != nullptr &&
                std::to_integer<unsigned>(which.bytes()[0]) == 0xFE &&
                (std::to_integer<unsigned>(which.bytes()[1]) & 0xC0) == 0x80)
                which.scope(::if_nametoindex(one->ifa_name));

            if (std::find(made.begin(), made.end(), which) == made.end()) made.push_back(which);
        }

#endif

        giving(std::move(made));
    }};
}

// `Trouble` does not translate the number by itself. The numbers come from three different worlds
// (errno, WSA and EAI), and the same value means something different in each. The sentence is put
// together by whoever knows where the number came from; `spoken()` turns a number into a sentence.
Trouble::Trouble(const std::string& said, int code, Address where)
    : talcite::Trouble(said, code), where_(std::move(where)) {}

// The system's description of an error number; empty for 0. A number without a sentence next to it
// is useless in a log, and the sentence from the system is in its language, as it should be, since
// it is the system that complains, not us.
std::string spoken(int code) {
    if (code == 0) return {};

#ifdef _WIN32
    char* said = nullptr;

    const DWORD much = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(code), 0, reinterpret_cast<char*>(&said), 0, nullptr);

    if (much == 0 || said == nullptr) return std::to_string(code);

    std::string made{said, much};

    LocalFree(said);

    while (!made.empty() && (made.back() == '\r' || made.back() == '\n' || made.back() == ' '))
        made.pop_back();

    return made;
#else
    return std::strerror(code);
#endif
}

}
