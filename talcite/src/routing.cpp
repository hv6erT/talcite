#include "talcite/router.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "talcite/http.hpp"
#include "talcite/random.hpp"
#include "talcite/trouble.hpp"
#include "talcite/udp.hpp"

#include "inside.hpp"
#include "routing.hpp"
#include "starting.hpp"

#ifdef _WIN32
#include <winsock2.h>

#include <ws2tcpip.h>

#include <iphlpapi.h>
#else
#include <fstream>
#include <sstream>
#endif

namespace talcite::net {

// A port opened on the router: the sharing side listens, and only the router lets it into the home
// network from outside. Three ways, in order:
//
// - PCP (RFC 6887): the newest, UDP to port 5351 of the router; a MAP request with a random nonce,
//   which later renews and closes the same mapping.
// - NAT-PMP (RFC 6886): the predecessor of PCP, on the same port. A router that knows only NAT-PMP
//   answers PCP with "unsupported version" in the NAT-PMP format, and only then do we go this way.
//   A router silent on PCP would be silent here too, so we do not bother it.
// - UPnP IGD: the most common in home routers: SSDP (M-SEARCH to 239.255.255.250:1900 and directly
//   to the router), the device description over HTTP, then SOAP to the WANIPConnection or
//   WANPPPConnection service. The description and the answers are read by the XML seam; talcite
//   has no parser of its own.
//
// The question runs on a pool thread and blocks it, like the STUN scout: there is one, asked
// rarely (opening, renewal, closing), and waiting for a router usually takes milliseconds. The
// router object remembers which way worked and starts from it at renewal; it also remembers the
// PCP nonce and the UPnP service, without which a mapping can be neither renewed nor closed.
//
// Nothing here sets anything permanently without the caller knowing: a mapping has a lifetime and
// expires by itself, unless a UPnP router knows only permanent mappings; then `lasting` comes back
// as zero and closing is up to the caller.

// The default router from the Linux routing table (the text of /proc/net/route): the gateway of
// the default route with the lowest metric. It is separate so that a test can pass the table text
// directly, without depending on the network of the machine it runs on.
std::optional<Address> routed(std::string_view table) {
    std::optional<Address> best;
    unsigned long lowest = 0;

    std::size_t at = table.find('\n');

    while (at != std::string_view::npos && at < table.size()) {
        const std::size_t end = table.find('\n', at + 1);
        const std::string_view line = table.substr(at + 1, end == std::string_view::npos ? std::string_view::npos : end - at - 1);

        at = end;

        std::vector<std::string> fields;
        std::string field;

        for (const char one : line) {
            if (one == ' ' || one == '\t' || one == '\r') {
                if (!field.empty()) fields.push_back(std::move(field));

                field.clear();
            } else {
                field += one;
            }
        }

        if (!field.empty()) fields.push_back(std::move(field));

        // Iface, Destination, Gateway, Flags, RefCnt, Use, Metric, Mask… A default route has a zero
        // mask (its destination must then be zero too), and a route through a router has a nonzero
        // gateway.
        if (fields.size() < 8 || fields[7] != "00000000") continue;

        const auto number = [](const std::string& hex) -> std::optional<unsigned long> {
            if (hex.empty() || hex.size() > 8 || !std::all_of(hex.begin(), hex.end(), [](char c) {
                    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
                }))
                return std::nullopt;

            return std::stoul(hex, nullptr, 16);
        };

        const std::optional<unsigned long> through = number(fields[2]);

        if (!through || *through == 0) continue;

        const std::string& metric_text = fields[6];

        if (metric_text.empty() || !std::all_of(metric_text.begin(), metric_text.end(), [](char c) { return c >= '0' && c <= '9'; }))
            continue;

        const unsigned long metric = std::stoul(metric_text);

        if (best && metric >= lowest) continue;

        // The kernel prints the address as a number in memory order, so copying the bytes gives
        // network order on every machine, not only on a little-endian one.
        const auto value = static_cast<std::uint32_t>(*through);

        std::array<std::byte, 16> raw{};

        std::memcpy(raw.data(), &value, 4);

        best = Address{Family::four, raw, 0};
        lowest = metric;
    }

    return best;
}

// The IPv4 address of this computer's default router; rejects with `Trouble` when there is none
// or the system does not tell.
Promise<Address> gateway() {
    return Promise<Address>{[](auto giving, auto amiss) {
        starting();

#ifdef _WIN32
        // The best route to a public address: nothing is sent, the system only answers from its
        // table.
        IN_ADDR toward{};

        ::inet_pton(AF_INET, "8.8.8.8", &toward);

        MIB_IPFORWARDROW row{};

        const DWORD code = ::GetBestRoute(toward.S_un.S_addr, 0, &row);

        if (code != NO_ERROR || row.dwForwardNextHop == 0 || row.dwForwardType != MIB_IPROUTE_TYPE_INDIRECT) {
            amiss(std::make_exception_ptr(Trouble{"This computer has no router.", static_cast<int>(code)}));

            return;
        }

        std::array<std::byte, 16> raw{};

        std::memcpy(raw.data(), &row.dwForwardNextHop, 4);

        giving(Address{Family::four, raw, 0});
#elif defined(__linux__)
        std::ifstream in{"/proc/net/route"};
        std::stringstream read;

        read << in.rdbuf();

        if (const std::optional<Address> made = routed(read.str())) {
            giving(*made);

            return;
        }

        amiss(std::make_exception_ptr(Trouble{"This computer has no router."}));
#else
        amiss(std::make_exception_ptr(Trouble{"This system does not tell its router."}));
#endif
    }};
}

// Which of our addresses leads toward the given one: a connected UDP socket sends nothing, and the
// system picks its source address from the routing table, the same one a real conversation would
// use. The result has port zero.
Promise<Address> leaving(Address toward) {
    return Promise<Address>{[toward](auto giving, auto amiss) mutable {
        if (toward.family() != Family::four && toward.family() != Family::six) {
            amiss(std::make_exception_ptr(Trouble{"Only an IP address has a way out."}));

            return;
        }

        if (toward.port() == 0) toward.port(9);

        const Handle one = opened(toward.family(), false);

        if (one == nothing) {
            amiss(std::make_exception_ptr(Trouble{"Cannot open a socket."}));

            return;
        }

        Address made = joined(one, toward) ? named(one, false) : Address{};

        ending(one);

        if (!made.held()) {
            amiss(std::make_exception_ptr(Trouble{"No way leads to " + toward.text() + ".", 0, toward}));

            return;
        }

        made.port(0);

        giving(made);
    }};
}

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint16_t pcp_port = 5351;

// The longest UPnP lease, in seconds; IGD version 1 does not accept more.
constexpr std::uint32_t upnp_longest = 604800;

// The time left until `until`, rounded up to a whole millisecond and at least one, so that it can be
// passed as a patience. Rounding down would let a wait end before `until`, and the caller, seeing
// time left, would start another round in the last fraction of a millisecond.
Patience left(Clock::time_point until) {
    const auto rest = std::chrono::ceil<Patience>(until - Clock::now());

    return rest.count() > 0 ? rest : Patience{1};
}

// Appends the low 16 bits of `value` in network byte order.
void put16(Bytes& into, std::uint32_t value) {
    into.push_back(static_cast<std::byte>((value >> 8) & 0xff));
    into.push_back(static_cast<std::byte>(value & 0xff));
}

// Appends `value` as 32 bits in network byte order.
void put32(Bytes& into, std::uint32_t value) {
    put16(into, value >> 16);
    put16(into, value & 0xffff);
}

// The byte at `at`.
std::uint32_t got8(const Bytes& from, std::size_t at) { return std::to_integer<std::uint32_t>(from[at]); }

// The big-endian 16-bit number at `at`.
std::uint32_t got16(const Bytes& from, std::size_t at) { return (got8(from, at) << 8) | got8(from, at + 1); }

// The big-endian 32-bit number at `at`.
std::uint32_t got32(const Bytes& from, std::size_t at) { return (got16(from, at) << 16) | got16(from, at + 2); }

// Appends an address as a PCP field: IPv6 as is, IPv4 as ::ffff:a.b.c.d.
void put_address(Bytes& into, const Address& one) {
    if (one.family() == Family::six) {
        into.insert(into.end(), one.bytes().begin(), one.bytes().end());

        return;
    }

    for (int at = 0; at < 10; ++at) into.push_back(std::byte{0});

    into.push_back(std::byte{0xff});
    into.push_back(std::byte{0xff});
    into.insert(into.end(), one.bytes().begin(), one.bytes().begin() + 4);
}

// The 16-byte PCP address field at `at`, with `port`: an IPv4-mapped address becomes IPv4.
Address taken_address(const Bytes& from, std::size_t at, std::uint16_t port) {
    std::array<std::byte, 16> raw{};

    std::copy(from.begin() + static_cast<std::ptrdiff_t>(at), from.begin() + static_cast<std::ptrdiff_t>(at + 16), raw.begin());

    const bool four = std::all_of(raw.begin(), raw.begin() + 10, [](std::byte one) { return one == std::byte{0}; }) &&
                      raw[10] == std::byte{0xff} && raw[11] == std::byte{0xff};

    if (!four) return Address{Family::six, raw, port};

    std::array<std::byte, 16> short_one{};

    std::copy(raw.begin() + 12, raw.end(), short_one.begin());

    return Address{Family::four, short_one, port};
}

// The meaning of a PCP result code (RFC 6887, section 7.4), for error messages.
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

// The meaning of a NAT-PMP result code (RFC 6886, section 3.5), for error messages.
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

// A request with retries: a quarter of a second, then doubling, until the deadline. `judged` gets
// every datagram from the router and says whether that is the end; foreign sources and oversized
// datagrams are skipped silently. Returns whether `judged` accepted an answer.
//
// The rounds end at moments fixed from the first send (250, 750, 1750 ms, ...), not from the clock
// after each wait: the number of sends then follows from the deadline alone. A wait that ends a
// little early or late moves no round, and the last round, which ends at the deadline, is the last.
template <typename Judge>
bool asked(const Udp& post, const Address& where, const Bytes& what, Clock::time_point until, Judge judged) {
    Patience wait{250};
    Clock::time_point edge = Clock::now();

    while (edge < until) {
        post.send(where, what, left(until)).await();

        edge = std::min(edge + wait, until);

        while (Clock::now() < edge) {
            Datagram came;

            try {
                came = post.receive(1500, left(edge)).await();
            } catch (const Overdue&) {
                break;
            } catch (const Trouble&) {
                continue;
            }

            if (came.from != where) continue;

            if (judged(came.bytes)) return true;
        }

        wait *= 2;
    }

    return false;
}

struct Control {
    Url url;
    std::string service;
};

}

struct Router::Held {
    Routing routing;

    std::mutex guard;
    std::map<std::pair<Carrier, std::uint16_t>, Bytes> nonces;
    std::optional<Opener> worked;
    std::optional<Control> control;
};

namespace {

using Kept = std::shared_ptr<Router::Held>;

// The time for one UDP way: 250 + 500 + 1000 ms. A home router answers within milliseconds; one
// that stays silent for almost two seconds does not know this way.
constexpr Patience udp_turn{1750};

// The router's address for PCP and NAT-PMP: the gateway, on port 5351 unless a port is given.
Address aimed(const Routing& routing) {
    Address made = routing.gateway;

    if (made.port() == 0) made.port(pcp_port);

    return made;
}

// The earlier of two moments.
Clock::time_point sooner(Clock::time_point one, Clock::time_point two) { return std::min(one, two); }

// --- PCP ------------------------------------------------------------------------------------------

struct PcpOutcome {
    std::optional<Opening> opened;
    bool old = false;
};

// The PCP nonce of the mapping for this carrier and inside port, drawn at random the first time.
// The same nonce renews and closes the mapping.
Bytes nonce_for(const Kept& held, Carrier carrier, std::uint16_t inside) {
    const std::lock_guard<std::mutex> kept{held->guard};

    Bytes& one = held->nonces[{carrier, inside}];

    if (one.empty()) one = random(12);

    return one;
}

// One PCP MAP request (RFC 6887, section 11); a lifetime of zero deletes the mapping. Returns the
// opening, `old` when the router knows only NAT-PMP, or nothing when it does not answer; throws
// `Trouble` when it refuses.
PcpOutcome pcp(const Kept& held, const Address& mine, Carrier carrier, std::uint16_t inside, std::uint16_t outside,
               std::uint32_t lifetime, Clock::time_point until) {
    const Bytes nonce = nonce_for(held, carrier, inside);
    const std::uint32_t protocol = carrier == Carrier::tcp ? 6 : 17;

    Bytes what;

    what.push_back(std::byte{2});
    what.push_back(std::byte{1});
    put16(what, 0);
    put32(what, lifetime);
    put_address(what, mine);
    what.insert(what.end(), nonce.begin(), nonce.end());
    what.push_back(static_cast<std::byte>(protocol));
    what.push_back(std::byte{0});
    what.push_back(std::byte{0});
    what.push_back(std::byte{0});
    put16(what, inside);
    put16(what, outside);
    put_address(what, Address{Family::four, {}, 0});

    const Udp post{Address::any(mine.family(), 0)};

    PcpOutcome made;
    std::optional<Trouble> refused;

    const bool answered = asked(post, aimed(held->routing), what, until, [&](const Bytes& came) {
        // A router with only NAT-PMP: "unsupported version" in its format.
        if (came.size() >= 4 && came[0] == std::byte{0}) {
            if (got16(came, 2) == 1) {
                made.old = true;

                return true;
            }

            return false;
        }

        if (came.size() < 60 || came[0] != std::byte{2} || came[1] != std::byte{0x81}) return false;

        if (!std::equal(nonce.begin(), nonce.end(), came.begin() + 24)) return false;

        if (const std::uint32_t code = got8(came, 3); code != 0) {
            refused.emplace("The router refused the port (PCP: " + pcp_said(code) + ").", static_cast<int>(code));

            return true;
        }

        if (got8(came, 36) != protocol || got16(came, 40) != inside) return false;

        made.opened = Opening{Opener::pcp, carrier, inside, taken_address(came, 44, static_cast<std::uint16_t>(got16(came, 42))),
                              std::chrono::seconds{got32(came, 4)}};

        return true;
    });

    if (refused) throw *refused;

    if (!answered) return {};

    return made;
}

// --- NAT-PMP --------------------------------------------------------------------------------------

// One NAT-PMP mapping request (RFC 6886, section 3.3), followed, when opening, by a question for
// the external address; a lifetime of zero deletes the mapping. Returns nothing when the router
// does not answer and throws `Trouble` when it refuses.
std::optional<Opening> pmp(const Kept& held, Carrier carrier, std::uint16_t inside, std::uint16_t outside,
                           std::uint32_t lifetime, Clock::time_point until) {
    const std::uint32_t op = carrier == Carrier::udp ? 1 : 2;
    const Address where = aimed(held->routing);
    const Udp post{Address::any(Family::four, 0)};

    Bytes what;

    what.push_back(std::byte{0});
    what.push_back(static_cast<std::byte>(op));
    put16(what, 0);
    put16(what, inside);
    put16(what, outside);
    put32(what, lifetime);

    std::optional<Trouble> refused;
    std::optional<Opening> made;

    const bool answered = asked(post, where, what, until, [&](const Bytes& came) {
        if (came.size() < 16 || came[0] != std::byte{0} || got8(came, 1) != 128 + op) return false;

        if (const std::uint32_t code = got16(came, 2); code != 0) {
            refused.emplace("The router refused the port (NAT-PMP: " + pmp_said(code) + ").", static_cast<int>(code));

            return true;
        }

        if (got16(came, 8) != inside) return false;

        made = Opening{Opener::pmp, carrier, inside, Address{Family::four, {}, static_cast<std::uint16_t>(got16(came, 10))},
                       std::chrono::seconds{got32(came, 12)}};

        return true;
    });

    if (refused) throw *refused;

    if (!answered || !made) return std::nullopt;

    // Closing has no external address, so we ask only when opening.
    if (lifetime == 0) return made;

    const std::uint16_t port = made->outside.port();

    (void)asked(post, where, Bytes{std::byte{0}, std::byte{0}}, sooner(until, Clock::now() + udp_turn), [&](const Bytes& came) {
        if (came.size() < 12 || came[0] != std::byte{0} || got8(came, 1) != 128 || got16(came, 2) != 0) return false;

        std::array<std::byte, 16> raw{};

        std::copy(came.begin() + 8, came.begin() + 12, raw.begin());

        made->outside = Address{Family::four, raw, port};

        return true;
    });

    return made;
}

// --- UPnP IGD -------------------------------------------------------------------------------------

// The services that open ports, best first.
constexpr std::array<std::string_view, 3> services{"urn:schemas-upnp-org:service:WANIPConnection:2",
                                                   "urn:schemas-upnp-org:service:WANIPConnection:1",
                                                   "urn:schemas-upnp-org:service:WANPPPConnection:1"};

constexpr std::array<std::string_view, 3> searched{"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                                                   "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                                                   "urn:schemas-upnp-org:service:WANIPConnection:1"};

// An ASCII letter in lower case; anything else unchanged.
char lowered(char one) { return one >= 'A' && one <= 'Z' ? static_cast<char>(one - 'A' + 'a') : one; }

// The text without leading and trailing spaces, tabs and line ends.
std::string trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
        text.remove_prefix(1);

    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
        text.remove_suffix(1);

    return std::string{text};
}

// The description address from an SSDP answer: the LOCATION header, in any letter case.
std::optional<std::string> located(const Bytes& came) {
    const std::string text = talcite::text(came);

    if (text.rfind("HTTP/1.1 200", 0) != 0 && text.rfind("HTTP/1.0 200", 0) != 0) return std::nullopt;

    std::size_t at = 0;

    while ((at = text.find('\n', at)) != std::string::npos) {
        ++at;

        const std::size_t end = text.find('\n', at);
        const std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        const std::size_t colon = line.find(':');

        if (colon == std::string::npos) continue;

        std::string name = trimmed(std::string_view{line}.substr(0, colon));

        std::transform(name.begin(), name.end(), name.begin(), [](char one) { return lowered(one); });

        if (name == "location") return trimmed(std::string_view{line}.substr(colon + 1));
    }

    return std::nullopt;
}

// The router descriptions found by SSDP: first those that lie on the router itself, because a
// network may also hold repeaters and other devices that pose as a gateway, and only the real one
// opens the port.
std::vector<Url> discovered(const Kept& held, Clock::time_point until) {
    const Routing& routing = held->routing;
    const Udp post{Address::any(Family::four, 0)};

    Address direct = routing.gateway;

    direct.port(1900);

    std::vector<Address> targets{routing.discovery};

    if (direct != routing.discovery) targets.push_back(direct);

    for (const std::string_view one : searched) {
        const std::string message = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: " +
                                    std::string{one} + "\r\n\r\n";

        for (const Address& to : targets) {
            try {
                post.send(to, bytes(message), left(until)).await();
            } catch (const Trouble&) {
                // A network without multicast; the direct question remains.
            }
        }
    }

    std::vector<Url> made;
    const std::string mine = routing.gateway.text();
    const Clock::time_point edge = sooner(until, Clock::now() + Patience{1500});

    while (Clock::now() < edge) {
        Datagram came;

        try {
            came = post.receive(4096, left(edge)).await();
        } catch (const Overdue&) {
            break;
        } catch (const Trouble&) {
            continue;
        }

        const std::optional<std::string> where = located(came.bytes);

        if (!where) continue;

        const std::optional<Url> url = Url::from_string(*where);

        if (!url || url->secure || std::find(made.begin(), made.end(), *url) != made.end()) continue;

        if (url->host == mine) {
            made.insert(made.begin(), *url);

            break;
        }

        made.push_back(*url);
    }

    return made;
}

// The local part of an XML name, without its namespace prefix.
std::string_view local(std::string_view name) {
    const std::size_t at = name.rfind(':');

    return at == std::string_view::npos ? name : name.substr(at + 1);
}

// Collects every element named `name` (ignoring the prefix) in the subtree of `one`.
void gathered(const xml::Node& one, std::string_view name, std::vector<const xml::Node*>& into) {
    if (one.kind != xml::Kind::element) return;

    if (local(one.name) == name) into.push_back(&one);

    for (const xml::Node& each : one.children) gathered(each, name, into);
}

// Every element named `name` in the document.
std::vector<const xml::Node*> found(const xml::Document& document, std::string_view name) {
    std::vector<const xml::Node*> made;

    for (const xml::Node& one : document.nodes) gathered(one, name, made);

    return made;
}

// The first child element of `one` named `name`, or null.
const xml::Node* inner(const xml::Node& one, std::string_view name) {
    for (const xml::Node& each : one.children)
        if (each.kind == xml::Kind::element && local(each.name) == name) return &each;

    return nullptr;
}

// The trimmed text of the first element named `name`, or an empty string.
std::string said_in(const xml::Document& document, std::string_view name) {
    const std::vector<const xml::Node*> all = found(document, name);

    return all.empty() ? std::string{} : trimmed(xml::text(*all.front()));
}

// The service from a device description: the best of the supported ones, with its control address
// resolved against URLBase, if the description has it, or against the address of the description
// itself.
std::optional<Control> controlled(const xml::Document& document, const Url& from) {
    Url base = from;

    if (const std::string told = said_in(document, "URLBase"); !told.empty())
        if (const std::optional<Url> made = Url::from_string(told)) base = *made;

    const std::vector<const xml::Node*> all = found(document, "service");

    for (const std::string_view wanted : services) {
        for (const xml::Node* one : all) {
            const xml::Node* type = inner(*one, "serviceType");
            const xml::Node* control = inner(*one, "controlURL");

            if (type == nullptr || control == nullptr || trimmed(xml::text(*type)) != wanted) continue;

            const std::optional<Url> url = base.resolved(trimmed(xml::text(*control)));

            if (!url || url->secure) continue;

            return Control{*url, std::string{wanted}};
        }
    }

    return std::nullopt;
}

// The text with the five XML special characters escaped.
std::string escaped(std::string_view text) {
    std::string made;

    for (const char one : text) {
        switch (one) {
            case '&': made += "&amp;"; break;
            case '<': made += "&lt;"; break;
            case '>': made += "&gt;"; break;
            case '"': made += "&quot;"; break;
            case '\'': made += "&apos;"; break;
            default: made += one;
        }
    }

    return made;
}

// A SOAP call; returns the decoded answer. A refusal by the service is HTTP 500 with UPnPError; its
// number goes into the `Trouble`, because the caller decides 718 (port taken) and 725 (only
// permanent) by itself.
xml::Document soap(const Kept& held, const Control& control, std::string_view action,
                   const std::vector<std::pair<std::string_view, std::string>>& arguments, Clock::time_point until) {
    std::string body = "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                       "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" +
                       std::string{action} + " xmlns:u=\"" + control.service + "\">";

    for (const auto& [name, value] : arguments)
        body += "<" + std::string{name} + ">" + escaped(value) + "</" + std::string{name} + ">";

    body += "</u:" + std::string{action} + "></s:Body></s:Envelope>\r\n";

    Limits limits;

    limits.patience = left(until);
    limits.body = 256 * 1024;

    const HttpClient client{Seam<TlsProvider>{}, limits};

    const Response got =
        client
            .post(Request{control.url,
                          {{"Content-Type", "text/xml; charset=\"utf-8\""},
                           {"SOAPAction", "\"" + control.service + "#" + std::string{action} + "\""}}},
                  bytes(body))
            .await();

    if (got.status != 200 && got.status != 500)
        throw Trouble{"The router answered UPnP with HTTP " + std::to_string(got.status) + "."};

    const xml::Document made = held->routing.xml.decoded(got.body);

    if (got.status == 200) return made;

    const std::string code = said_in(made, "errorCode");
    const std::string description = said_in(made, "errorDescription");

    int number = 0;

    if (!code.empty() && code.size() <= 6 && std::all_of(code.begin(), code.end(), [](char one) { return one >= '0' && one <= '9'; }))
        number = std::stoi(code);

    throw Trouble{"The router refused the port (UPnP: " + (description.empty() ? "error " + code : description) + ").",
                  number};
}

// The UPnP service found earlier, if any.
std::optional<Control> known_control(const Kept& held) {
    const std::lock_guard<std::mutex> kept{held->guard};

    return held->control;
}

// The UPnP service: the one found earlier, or the first that a description found by SSDP offers,
// which is then remembered.
std::optional<Control> found_control(const Kept& held, Clock::time_point until) {
    if (std::optional<Control> known = known_control(held)) return known;

    Limits limits;

    limits.body = 256 * 1024;

    for (const Url& one : discovered(held, until)) {
        if (Clock::now() >= until) break;

        limits.patience = left(until);

        try {
            const Response got = HttpClient{Seam<TlsProvider>{}, limits}.get(Request{one, {}}).await();

            if (got.status != 200) continue;

            if (std::optional<Control> made = controlled(held->routing.xml.decoded(got.body), got.url)) {
                const std::lock_guard<std::mutex> kept{held->guard};

                held->control = made;

                return made;
            }
        } catch (const std::exception&) {
            // A malformed description or a device that does not answer; we try the next one.
        }
    }

    return std::nullopt;
}

// Opens the port through UPnP IGD with AddPortMapping, then asks for the external address. Returns
// nothing without an XML seam or a service; throws `Trouble` when the router refuses.
std::optional<Opening> upnp(const Kept& held, const Address& mine, Carrier carrier, std::uint16_t inside,
                            std::uint32_t lifetime, Clock::time_point until) {
    if (!held->routing.xml.bound()) return std::nullopt;

    const std::optional<Control> control = found_control(held, until);

    if (!control) return std::nullopt;

    const std::string protocol = carrier == Carrier::tcp ? "TCP" : "UDP";

    std::uint32_t lease = std::min(lifetime, upnp_longest);
    std::uint16_t outside = inside;

    for (int turn = 0;; ++turn) {
        try {
            (void)soap(held, *control, "AddPortMapping",
                       {{"NewRemoteHost", ""},
                        {"NewExternalPort", std::to_string(outside)},
                        {"NewProtocol", protocol},
                        {"NewInternalPort", std::to_string(inside)},
                        {"NewInternalClient", mine.text()},
                        {"NewEnabled", "1"},
                        {"NewPortMappingDescription", held->routing.label},
                        {"NewLeaseDuration", std::to_string(lease)}},
                       until);

            break;
        } catch (const Trouble& why) {
            // 725: the router knows only permanent mappings. 718: the external port is taken, so we
            // take another one.
            if (why.code() == 725 && lease != 0 && turn < 6) {
                lease = 0;

                continue;
            }

            if (why.code() == 718 && turn < 6) {
                const std::uint16_t taken = outside;

                while (outside == taken) {
                    const Bytes drawn = random(2);

                    outside = static_cast<std::uint16_t>(
                        1024 + ((std::to_integer<unsigned>(drawn[0]) << 8 | std::to_integer<unsigned>(drawn[1])) % (65536 - 1024)));
                }

                continue;
            }

            throw;
        }
    }

    Address seen{Family::four, {}, outside};

    try {
        const std::string told = said_in(soap(held, *control, "GetExternalIPAddress", {}, until), "NewExternalIPAddress");

        if (!told.empty()) seen = Address{told, outside};
    } catch (const std::exception&) {
        // The external address is extra information; the port is open anyway.
    }

    return Opening{Opener::upnp, carrier, inside, seen, std::chrono::seconds{lease}};
}

// --- The whole ------------------------------------------------------------------------------------

// Our address in the home network: the one given in the settings, or the one leading to the
// router.
Address ours(const Kept& held) {
    if (held->routing.mine.held()) return held->routing.mine;

    return leaving(held->routing.gateway).await();
}

// Remembers which way worked, so that the next opening starts from it.
void remembered(const Kept& held, Opener one) {
    const std::lock_guard<std::mutex> kept{held->guard};

    held->worked = one;
}

// Opens the port: first the way that worked last time, otherwise PCP, NAT-PMP when the router
// knows only that, and UPnP. Throws the first refusal, or `Trouble` when no router answered.
Opening opening(const Kept& held, Carrier carrier, std::uint16_t inside, std::chrono::seconds lasting) {
    const Clock::time_point until = Clock::now() + held->routing.patience;
    const Address mine = ours(held);
    const auto lifetime = static_cast<std::uint32_t>(std::clamp<std::chrono::seconds::rep>(lasting.count(), 1, 0x7fffffff));

    std::optional<Opener> worked;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        worked = held->worked;
    }

    // The first refusal is kept: "the router refused" says more than "nobody answered".
    std::exception_ptr first;

    const auto tried = [&](auto&& doing) -> std::optional<Opening> {
        try {
            return doing();
        } catch (const talcite::Trouble&) {
            if (!first) first = std::current_exception();
        }

        return std::nullopt;
    };

    const auto by_pmp = [&] {
        return tried([&] { return pmp(held, carrier, inside, inside, lifetime, sooner(until, Clock::now() + udp_turn)); });
    };

    const auto by_upnp = [&] { return tried([&] { return upnp(held, mine, carrier, inside, lifetime, until); }); };

    std::optional<Opening> made;

    if (worked == Opener::pmp) made = by_pmp();
    if (!made && worked == Opener::upnp) made = by_upnp();

    if (!made && worked != Opener::pmp && worked != Opener::upnp) {
        bool old = false;

        made = tried([&]() -> std::optional<Opening> {
            const PcpOutcome got = pcp(held, mine, carrier, inside, inside, lifetime, sooner(until, Clock::now() + udp_turn));

            old = got.old;

            return got.opened;
        });

        if (!made && old) made = by_pmp();
    }

    if (!made && worked != Opener::upnp) made = by_upnp();

    if (made) {
        remembered(held, made->by);

        return *made;
    }

    if (first) std::rethrow_exception(first);

    throw Trouble{"No router opened the port: none answered PCP, NAT-PMP or UPnP."};
}

// Closes a mapping the same way it was opened; throws `Trouble` when this router did not open it
// or does not answer.
void closing(const Kept& held, const Opening& one) {
    const Clock::time_point until = Clock::now() + held->routing.patience;

    switch (one.by) {
        case Opener::pcp: {
            {
                const std::lock_guard<std::mutex> kept{held->guard};

                if (held->nonces.count({one.carrier, one.inside}) == 0)
                    throw Trouble{"This router did not open that port."};
            }

            if (!pcp(held, ours(held), one.carrier, one.inside, 0, 0, until).opened)
                throw Trouble{"The router did not answer the closing."};

            const std::lock_guard<std::mutex> kept{held->guard};

            held->nonces.erase({one.carrier, one.inside});

            return;
        }
        case Opener::pmp:
            if (!pmp(held, one.carrier, one.inside, 0, 0, until)) throw Trouble{"The router did not answer the closing."};

            return;
        case Opener::upnp: {
            const std::optional<Control> control = known_control(held);

            if (!control) throw Trouble{"This router did not open that port."};

            (void)soap(held, *control, "DeletePortMapping",
                       {{"NewRemoteHost", ""},
                        {"NewExternalPort", std::to_string(one.outside.port())},
                        {"NewProtocol", one.carrier == Carrier::tcp ? "TCP" : "UDP"}},
                       until);

            return;
        }
    }
}

// A promise already rejected with `Trouble` carrying `said`.
template <typename T>
Promise<T> refused(const char* said) {
    return Promise<T>{[said](auto, auto reject) { reject(std::make_exception_ptr(Trouble{said})); }};
}

}

// A router described by `routing`: its address, discovery, our address, the label of mappings,
// the patience and the XML seam.
Router::Router(Routing routing) : held_(std::make_shared<Held>()) { held_->routing = std::move(routing); }

// Opens `inside` on the router for `lasting`, in the background; resolves with the opening.
Promise<Opening> Router::open(Carrier carrier, std::uint16_t inside, std::chrono::seconds lasting) const {
    if (!held_) return refused<Opening>("The router is empty.");

    if (inside == 0) return refused<Opening>("Port zero cannot be opened.");

    if (!held_->routing.gateway.held()) return refused<Opening>("The router has no address.");

    Promise<Opening> made{[held = held_, carrier, inside, lasting](auto giving, auto amiss) {
        try {
            giving(opening(held, carrier, inside, lasting));
        } catch (...) {
            amiss(std::current_exception());
        }
    }};

    made.start();

    return made;
}

// Closes an opening made by this router, in the background.
Promise<void> Router::close(Opening one) const {
    if (!held_) return refused<void>("The router is empty.");

    Promise<void> made{[held = held_, one](auto giving, auto amiss) {
        try {
            closing(held, one);

            giving();
        } catch (...) {
            amiss(std::current_exception());
        }
    }};

    made.start();

    return made;
}

}
