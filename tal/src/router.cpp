#include "tal/peer.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "tal/crypto.hpp"
#include "tal/trouble.hpp"

#include "netting.hpp"
#include "platform/offload.hpp"
#include "routing.hpp"

#ifdef _WIN32
#include <iphlpapi.h>
#endif

// A port opened on the router, ported from talcite's routing.cpp: the sharing side listens, and
// only the router lets strangers from outside into the home network. Two ways, in order:
//
// - PCP (RFC 6887): UDP to port 5351 of the router, a MAP request with a random nonce, which later
//   renews and closes the same mapping.
// - NAT-PMP (RFC 6886), the forerunner of PCP on the same port. A router that knows only NAT-PMP
//   answers PCP with "unsupported version" in its own format, and only then do we go this way; one
//   silent to PCP would be silent here too.
//
// UPnP IGD comes with our own XML (E5e). One way gets `Routing::turn`, sending at 0, 250, 750 ms
// and so on: a home router answers within milliseconds, and one silent for almost two seconds
// does not speak that way. The router remembers the way that worked, to start from it next time,
// and the PCP nonces, without which a mapping can be neither renewed nor closed. A mapping always
// has a lifetime and goes by itself.
namespace tal::net {

namespace routing {

// The default router of the Linux routing table `table` (the text of /proc/net/route): the gateway
// of a default route with the lowest metric; nothing without one.
std::optional<Address> routed(std::string_view table) {
    std::optional<Address> best;
    unsigned long lowest = 0;
    std::size_t at = table.find('\n');

    while (at != std::string_view::npos && at < table.size()) {
        const std::size_t end = table.find('\n', at + 1);
        const std::string_view line = table.substr(at + 1, end == std::string_view::npos ? std::string_view::npos : end - at - 1);
        std::vector<std::string> fields;
        std::string field;

        at = end;

        for (const char one : line) {
            if (one == ' ' || one == '\t' || one == '\r') {
                if (!field.empty()) fields.push_back(std::move(field));

                field.clear();
            } else {
                field += one;
            }
        }

        if (!field.empty()) fields.push_back(std::move(field));

        const auto hexadecimal = [](const std::string& text) {
            return !text.empty() && text.size() <= 8 && std::all_of(text.begin(), text.end(), [](char one) {
                       return (one >= '0' && one <= '9') || (one >= 'A' && one <= 'F') || (one >= 'a' && one <= 'f');
                   });
        };
        const auto decimal = [](const std::string& text) {
            return !text.empty() && text.size() <= 9 && std::all_of(text.begin(), text.end(), [](char one) { return one >= '0' && one <= '9'; });
        };

        if (fields.size() < 8 || fields[7] != "00000000" || !hexadecimal(fields[2]) || !decimal(fields[6])) continue;

        const auto through = static_cast<std::uint32_t>(std::stoul(fields[2], nullptr, 16));
        const unsigned long metric = std::stoul(fields[6]);

        if (through == 0 || (best && metric >= lowest)) continue;

        std::array<std::byte, 16> raw{};

        std::memcpy(raw.data(), &through, 4);

        best = Address{Family::four, raw, 0};
        lowest = metric;
    }

    return best;
}

}

// The IPv4 address of this computer's default router. Throws `Trouble` (missing) when there is
// none, and unsupported where the system does not tell.
Task<Address> gateway() {
    co_return co_await platform::offloaded([]() -> Address {
#ifdef _WIN32
        netting::starting();

        const std::uint8_t toward[4]{8, 8, 8, 8};
        DWORD public_one = 0;
        MIB_IPFORWARDROW row{};

        std::memcpy(&public_one, toward, 4);

        const DWORD code = ::GetBestRoute(public_one, 0, &row);

        if (code != NO_ERROR || row.dwForwardNextHop == 0 || row.dwForwardType != MIB_IPROUTE_TYPE_INDIRECT)
            throw Trouble{Kind::missing, "This computer has no router.", {}, static_cast<int>(code)};

        std::array<std::byte, 16> raw{};

        std::memcpy(raw.data(), &row.dwForwardNextHop, 4);

        return Address{Family::four, raw, 0};
#elif defined(__linux__)
        std::ifstream in{"/proc/net/route"};
        std::stringstream text;

        text << in.rdbuf();

        if (const std::optional<Address> made = routing::routed(text.str())) return *made;

        throw Trouble{Kind::missing, "This computer has no router."};
#else
        throw Trouble{Kind::unsupported, "This system does not tell its router."};
#endif
    });
}

// Which of our addresses leads toward `toward`, with port zero: a joined UDP socket sends nothing,
// and the system takes its own address from the routing table, the one a conversation would use.
// Throws `Trouble` (other) for what is not an IP address, missing when no way leads there.
Task<Address> leaving(Address toward) {
    if (toward.family() != Family::four && toward.family() != Family::six) throw Trouble{Kind::other, "Only an IP address has a way out."};

    if (toward.port() == 0) toward.port(9);

    co_return co_await platform::offloaded([toward]() -> Address {
        netting::starting();

        const netting::Handle one = netting::opened(toward.family(), false);

        if (one == netting::nothing) netting::failed("Cannot open a socket", netting::amiss(), toward);

        Address made = netting::joined(one, toward) ? netting::named(one, false) : Address{};

        netting::ending(one);

        if (!made.held()) throw Trouble{Kind::missing, "No way leads to " + toward.text() + ".", toward.spelled()};

        made.port(0);

        return made;
    });
}

namespace {

constexpr std::uint16_t pcp_port = 5351;

// Appends the low 16 bits of `value` to `into`, most significant first.
void put16(Bytes& into, std::uint32_t value) {
    into.push_back(static_cast<std::byte>((value >> 8) & 0xff));
    into.push_back(static_cast<std::byte>(value & 0xff));
}

// Appends `value` to `into` as 32 bits, most significant first.
void put32(Bytes& into, std::uint32_t value) {
    put16(into, value >> 16);
    put16(into, value & 0xffff);
}

// The byte at `at` of `from`.
std::uint32_t got8(const Bytes& from, std::size_t at) { return std::to_integer<std::uint32_t>(from[at]); }

// The big-endian 16-bit number at `at` of `from`.
std::uint32_t got16(const Bytes& from, std::size_t at) { return (got8(from, at) << 8) | got8(from, at + 1); }

// The big-endian 32-bit number at `at` of `from`.
std::uint32_t got32(const Bytes& from, std::size_t at) { return (got16(from, at) << 16) | got16(from, at + 2); }

// Appends the address `one` to `into` as PCP writes it: IPv6 as it is, IPv4 as ::ffff:a.b.c.d.
void put_address(Bytes& into, const Address& one) {
    if (one.family() == Family::six) {
        into.insert(into.end(), one.bytes().begin(), one.bytes().end());

        return;
    }

    into.insert(into.end(), 10, std::byte{0});
    into.push_back(std::byte{0xff});
    into.push_back(std::byte{0xff});
    into.insert(into.end(), one.bytes().begin(), one.bytes().begin() + 4);
}

// The PCP address of 16 bytes at `at` of `from`, with `port`; an IPv4-mapped one as IPv4.
Address taken_address(const Bytes& from, std::size_t at, std::uint16_t port) {
    std::array<std::byte, 16> raw{};

    std::copy(from.begin() + static_cast<std::ptrdiff_t>(at), from.begin() + static_cast<std::ptrdiff_t>(at + 16), raw.begin());

    const bool four = std::all_of(raw.begin(), raw.begin() + 10, [](std::byte one) { return one == std::byte{0}; }) && raw[10] == std::byte{0xff} && raw[11] == std::byte{0xff};

    if (!four) return Address{Family::six, raw, port};

    std::array<std::byte, 16> short_one{};

    std::copy(raw.begin() + 12, raw.end(), short_one.begin());

    return Address{Family::four, short_one, port};
}

// The meaning of the PCP result `code` (RFC 6887, 7.4).
std::string pcp_said(std::uint32_t code) {
    switch (code) {
        case 1: return "unsupported version";
        case 2: return "not authorized";
        case 3: return "malformed request";
        case 4: return "unsupported opcode";
        case 5: return "unsupported option";
        case 6: return "malformed option";
        case 7: return "network failure";
        case 8: return "no resources";
        case 9: return "unsupported protocol";
        case 10: return "user exceeded quota";
        case 11: return "cannot provide external";
        case 12: return "address mismatch";
        case 13: return "excessive remote peers";
        default: return "result " + std::to_string(code);
    }
}

// The meaning of the NAT-PMP result `code` (RFC 6886, 3.5).
std::string pmp_said(std::uint32_t code) {
    switch (code) {
        case 1: return "unsupported version";
        case 2: return "not authorized";
        case 3: return "network failure";
        case 4: return "out of resources";
        case 5: return "unsupported opcode";
        default: return "result " + std::to_string(code);
    }
}

// The trouble of a router that refused the port with `code` (2, not authorized, is denied).
Trouble refusal(std::uint32_t code, const std::string& said) {
    return Trouble{code == 2 ? Kind::denied : Kind::refused, "The router refused the port (" + said + ").", {}, static_cast<int>(code)};
}

// What a router's datagram is to a request: not its answer, or its answer.
enum class Verdict { foreign, answered };

// `what` sent to `where` from `post` and sent again at 250, 750, 1750 ms and so on from the first
// send, until `lasting` is over; `judged` hears every datagram from `where` and says whether it is
// the answer. Whether an answer came.
template <typename Judge>
Task<bool> asked(Udp post, Address where, Bytes what, std::chrono::milliseconds lasting, Judge judged) {
    const auto start = std::chrono::steady_clock::now();
    const auto until = start + lasting;
    std::chrono::milliseconds wait{250};
    auto edge = start;

    while (edge < until) {
        try {
            co_await post.send(where, what);
        } catch (const Trouble&) {
        }

        edge = std::min(edge + wait, until);

        for (auto now = std::chrono::steady_clock::now(); now < edge; now = std::chrono::steady_clock::now()) {
            Datagram came;

            try {
                came = co_await within(std::chrono::duration_cast<std::chrono::milliseconds>(edge - now) + std::chrono::milliseconds{1}, post.received(1500));
            } catch (const Trouble&) {
                break;
            }

            if (came.cut || came.from != where) continue;

            if (judged(came.bytes) == Verdict::answered) co_return true;
        }

        wait *= 2;
    }

    co_return false;
}

}

// What a router keeps: how to reach it, the way that worked, and the PCP nonce of each mapping.
struct Router::Held {
    Routing routing;
    std::mutex guard;
    std::map<std::pair<Carrier, std::uint16_t>, Bytes> nonces;
    std::optional<Opener> worked;
};

namespace {

using Kept = std::shared_ptr<Router::Held>;

// The router's address for PCP and NAT-PMP: the gateway, on 5351 unless it has a port.
Address aimed(const Routing& routing) {
    Address made = routing.gateway;

    if (made.port() == 0) made.port(pcp_port);

    return made;
}

// The PCP nonce of the mapping of `carrier` and `inside`, drawn the first time.
Bytes nonce_for(const Kept& held, Carrier carrier, std::uint16_t inside) {
    const std::lock_guard<std::mutex> kept{held->guard};
    Bytes& one = held->nonces[{carrier, inside}];

    if (one.empty()) one = crypto::random(12);

    return one;
}

// What one PCP request gave: the opening, or `old` for a router that knows only NAT-PMP.
struct PcpOutcome {
    std::optional<Opening> opened;
    bool old = false;
};

// One PCP MAP request (RFC 6887, 11) from `mine`; a lifetime of zero deletes the mapping. Nothing
// when the router stays silent; throws `Trouble` when it refuses.
Task<PcpOutcome> pcp(Kept held, Address mine, Carrier carrier, std::uint16_t inside, std::uint32_t lifetime) {
    const Bytes nonce = nonce_for(held, carrier, inside);
    const std::uint32_t protocol = carrier == Carrier::tcp ? 6 : 17;
    Bytes what{std::byte{2}, std::byte{1}};

    put16(what, 0);
    put32(what, lifetime);
    put_address(what, mine);
    what.insert(what.end(), nonce.begin(), nonce.end());
    what.push_back(static_cast<std::byte>(protocol));
    what.insert(what.end(), 3, std::byte{0});
    put16(what, inside);
    put16(what, lifetime == 0 ? 0 : inside);
    put_address(what, Address{Family::four, {}, 0});

    const Udp post = co_await Udp::bound(Address::any(mine.family(), 0));
    PcpOutcome made;
    std::optional<Trouble> refused;

    const bool answered = co_await asked(post, aimed(held->routing), what, held->routing.turn, [&](const Bytes& came) {
        if (came.size() >= 4 && came[0] == std::byte{0}) {
            if (got16(came, 2) != 1) return Verdict::foreign;

            made.old = true;

            return Verdict::answered;
        }

        if (came.size() < 60 || came[0] != std::byte{2} || came[1] != std::byte{0x81} || !std::equal(nonce.begin(), nonce.end(), came.begin() + 24))
            return Verdict::foreign;

        if (const std::uint32_t code = got8(came, 3); code != 0) {
            refused = refusal(code, "PCP: " + pcp_said(code));

            return Verdict::answered;
        }

        if (got8(came, 36) != protocol || got16(came, 40) != inside) return Verdict::foreign;

        made.opened = Opening{Opener::pcp, carrier, inside, taken_address(came, 44, static_cast<std::uint16_t>(got16(came, 42))), std::chrono::seconds{got32(came, 4)}};

        return Verdict::answered;
    });

    if (refused) throw *refused;

    co_return answered ? made : PcpOutcome{};
}

// One NAT-PMP mapping request (RFC 6886, 3.3), and when opening the question for the external
// address; a lifetime of zero deletes the mapping. Nothing when the router stays silent; throws
// `Trouble` when it refuses.
Task<std::optional<Opening>> pmp(Kept held, Carrier carrier, std::uint16_t inside, std::uint32_t lifetime) {
    const std::uint32_t op = carrier == Carrier::udp ? 1 : 2;
    const Address where = aimed(held->routing);
    const Udp post = co_await Udp::bound(Address::any(Family::four, 0));
    Bytes what{std::byte{0}, static_cast<std::byte>(op)};
    std::optional<Trouble> refused;
    std::optional<Opening> made;

    put16(what, 0);
    put16(what, inside);
    put16(what, lifetime == 0 ? 0 : inside);
    put32(what, lifetime);

    const bool answered = co_await asked(post, where, what, held->routing.turn, [&](const Bytes& came) {
        if (came.size() < 16 || came[0] != std::byte{0} || got8(came, 1) != 128 + op) return Verdict::foreign;

        if (const std::uint32_t code = got16(came, 2); code != 0) {
            refused = refusal(code, "NAT-PMP: " + pmp_said(code));

            return Verdict::answered;
        }

        if (got16(came, 8) != inside) return Verdict::foreign;

        made = Opening{Opener::pmp, carrier, inside, Address{Family::four, {}, static_cast<std::uint16_t>(got16(came, 10))}, std::chrono::seconds{got32(came, 12)}};

        return Verdict::answered;
    });

    if (refused) throw *refused;

    if (!answered || !made || lifetime == 0) co_return made;

    const std::uint16_t port = made->outside.port();

    (void)co_await asked(post, where, Bytes{std::byte{0}, std::byte{0}}, held->routing.turn, [&](const Bytes& came) {
        if (came.size() < 12 || came[0] != std::byte{0} || got8(came, 1) != 128 || got16(came, 2) != 0) return Verdict::foreign;

        std::array<std::byte, 16> raw{};

        std::copy(came.begin() + 8, came.begin() + 12, raw.begin());

        made->outside = Address{Family::four, raw, port};

        return Verdict::answered;
    });

    co_return made;
}

// Our address in the home network: the one given, or the one that leads to the router.
Task<Address> ours(Kept held) {
    if (held->routing.mine.held()) co_return held->routing.mine;

    co_return co_await leaving(held->routing.gateway);
}

// `inside` opened for `lasting`: the way that worked last time first, otherwise PCP, and NAT-PMP
// when the router knows only that. The first refusal comes out, or unsupported when nothing
// answered.
Task<Opening> opening(Kept held, Carrier carrier, std::uint16_t inside, std::chrono::seconds lasting) {
    const Address mine = co_await ours(held);
    const auto lifetime = static_cast<std::uint32_t>(std::clamp<std::chrono::seconds::rep>(lasting.count(), 1, 0x7fffffff));
    std::optional<Opener> worked;
    std::exception_ptr first;
    std::optional<Opening> made;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        worked = held->worked;
    }

    bool old = worked == Opener::pmp;

    if (worked != Opener::pmp) {
        try {
            const PcpOutcome got = co_await pcp(held, mine, carrier, inside, lifetime);

            made = got.opened;
            old = got.old;
        } catch (const Trouble&) {
            first = std::current_exception();
        }
    }

    if (!made && old) {
        try {
            made = co_await pmp(held, carrier, inside, lifetime);
        } catch (const Trouble&) {
            if (!first) first = std::current_exception();
        }
    }

    if (made) {
        const std::lock_guard<std::mutex> kept{held->guard};

        held->worked = made->by;

        co_return *made;
    }

    if (first) std::rethrow_exception(first);

    throw Trouble{Kind::unsupported, "No router answered PCP or NAT-PMP."};
}

// The mapping `one` closed the way it was opened. Throws `Trouble` (missing) when this router did
// not open it, overdue when the router does not answer.
Task<void> closing(Kept held, Opening one) {
    if (one.by == Opener::pmp) {
        if (!co_await pmp(held, one.carrier, one.inside, 0)) throw Trouble{Kind::overdue, "The router did not answer the closing."};

        co_return;
    }

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        if (held->nonces.count({one.carrier, one.inside}) == 0) throw Trouble{Kind::missing, "This router did not open that port."};
    }

    const Address mine = co_await ours(held);

    if (!(co_await pcp(held, mine, one.carrier, one.inside, 0)).opened) throw Trouble{Kind::overdue, "The router did not answer the closing."};

    const std::lock_guard<std::mutex> kept{held->guard};

    held->nonces.erase({one.carrier, one.inside});
}

}

// The router of `how`.
Router::Router(Routing how) : held_(std::make_shared<Held>()) { held_->routing = std::move(how); }

// `inside` opened on the router for `lasting`. Throws `Trouble` (other) for port zero or a router
// without an address.
Task<Opening> Router::open(Carrier carrier, std::uint16_t inside, std::chrono::seconds lasting) const {
    if (inside == 0) throw Trouble{Kind::other, "Port zero cannot be opened."};

    if (!held_->routing.gateway.held()) throw Trouble{Kind::other, "The router has no address."};

    return opening(held_, carrier, inside, lasting);
}

// The opening `one` of this router closed.
Task<void> Router::close(Opening one) const { return closing(held_, std::move(one)); }

}
