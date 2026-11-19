#include "tal/peer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
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

#include "tal/bytes.hpp"
#include "tal/crypto.hpp"
#include "tal/http.hpp"
#include "tal/trouble.hpp"
#include "tal/xml.hpp"

#include "netting.hpp"
#include "platform/offload.hpp"
#include "routing.hpp"

#ifdef _WIN32
#include <iphlpapi.h>
#endif

// A port opened on the router, ported from talcite's routing.cpp: the sharing side listens, and
// only the router lets strangers from outside into the home network. Three ways, in order:
//
// - PCP (RFC 6887): UDP to port 5351 of the router, a MAP request with a random nonce, which later
//   renews and closes the same mapping.
// - NAT-PMP (RFC 6886), the forerunner of PCP on the same port. A router that knows only NAT-PMP
//   answers PCP with "unsupported version" in its own format, and only then do we go this way; one
//   silent to PCP would be silent here too.
// - UPnP IGD, the most common in home routers: SSDP (M-SEARCH to `Routing::discovery` and straight
//   to the router), the description of the device over HTTP, then SOAP to its WANIPConnection or
//   WANPPPConnection service, read by our own XML.
//
// A UDP way gets `Routing::turn`, sending at 0, 250, 750 ms and so on: a home router answers within
// milliseconds, and one silent for almost two seconds does not speak that way; UPnP gets the rest
// of `Routing::patience`. The router remembers the way that worked, to start from it next time, the
// PCP nonces and the UPnP service, without which a mapping can be neither renewed nor closed. A
// mapping has a lifetime and goes by itself, unless a UPnP router knows only lasting ones: then
// `lasting` is zero and closing is the caller's.
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
    std::optional<std::pair<Url, std::string>> control;
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

// The longest UPnP lease in seconds; IGD version 1 takes no more.
constexpr std::uint32_t upnp_longest = 604800;

// The most a description or a SOAP answer may hold, and the limits of its XML.
constexpr std::size_t upnp_body = 256 * 1024;
constexpr xml::Limits upnp_xml{.bytes = upnp_body, .depth = 32, .nodes = 16384};

// The services that open ports, best first.
constexpr std::array<std::string_view, 3> services{"urn:schemas-upnp-org:service:WANIPConnection:2", "urn:schemas-upnp-org:service:WANIPConnection:1",
                                                   "urn:schemas-upnp-org:service:WANPPPConnection:1"};

// What SSDP asks for.
constexpr std::array<std::string_view, 3> searched{"urn:schemas-upnp-org:device:InternetGatewayDevice:1", "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                                                   "urn:schemas-upnp-org:service:WANIPConnection:1"};

// The milliseconds left until `until`, at least one.
std::chrono::milliseconds left(std::chrono::steady_clock::time_point until) {
    const auto now = std::chrono::steady_clock::now();

    return now >= until ? std::chrono::milliseconds{1} : std::chrono::duration_cast<std::chrono::milliseconds>(until - now) + std::chrono::milliseconds{1};
}

// `text` without the white space around it.
std::string_view trimmed(std::string_view text) {
    const std::size_t from = text.find_first_not_of(" \t\r\n");

    if (from == std::string_view::npos) return {};

    return text.substr(from, text.find_last_not_of(" \t\r\n") - from + 1);
}

// The address of the description in an SSDP answer `came`: its LOCATION header, in any letter case;
// nothing for anything but an answer of 200.
std::optional<std::string> located(const Bytes& came) {
    const std::string text = text::of(came);

    if (!text.starts_with("HTTP/1.1 200") && !text.starts_with("HTTP/1.0 200")) return std::nullopt;

    for (std::size_t at = text.find('\n'); at != std::string::npos; at = text.find('\n', at)) {
        ++at;

        const std::string_view line = std::string_view{text}.substr(at, text.find('\n', at) - at);
        const std::size_t colon = line.find(':');

        if (colon == std::string_view::npos) continue;

        std::string name{trimmed(line.substr(0, colon))};

        std::transform(name.begin(), name.end(), name.begin(), [](char one) { return one >= 'A' && one <= 'Z' ? static_cast<char>(one - 'A' + 'a') : one; });

        if (name == "location") return std::string{trimmed(line.substr(colon + 1))};
    }

    return std::nullopt;
}

// The descriptions SSDP found before `until`, at most a second and a half: the one on the router
// itself first, and then no more, since a network may hold repeaters and other devices that pose
// as a gateway, and only the real one opens the port. The question goes to `Routing::discovery`
// and straight to the router on 1900, unless the gateway has a port of its own, where it speaks PCP
// (as in the trials).
Task<std::vector<Url>> discovered(Kept held, std::chrono::steady_clock::time_point until) {
    const Routing& routing = held->routing;
    const Udp post = co_await Udp::bound(Address::any(Family::four, 0));
    Address direct = routing.gateway;
    std::vector<Address> targets{routing.discovery};

    direct.port(1900);

    if (routing.gateway.port() == 0 && direct != routing.discovery) targets.push_back(direct);

    for (const std::string_view one : searched) {
        const std::string message = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: " + std::string{one} + "\r\n\r\n";

        for (const Address& to : targets) {
            try {
                co_await post.send(to, bytes(message));
            } catch (const Trouble&) {
            }
        }
    }

    std::vector<Url> made;
    const std::string mine = routing.gateway.text();
    const auto edge = std::min(until, std::chrono::steady_clock::now() + std::chrono::milliseconds{1500});

    while (std::chrono::steady_clock::now() < edge) {
        Datagram came;

        try {
            came = co_await within(left(edge), post.received(4096));
        } catch (const Overdue&) {
            break;
        } catch (const Trouble&) {
            continue;
        }

        const std::optional<std::string> where = came.cut ? std::nullopt : located(came.bytes);
        const std::optional<Url> url = where ? Url::parsed(*where) : std::nullopt;

        if (!url || url->secure || std::find(made.begin(), made.end(), *url) != made.end()) continue;

        if (url->host == mine) {
            made.insert(made.begin(), *url);

            break;
        }

        made.push_back(*url);
    }

    co_return made;
}

// The local part of the XML name `name`, without its prefix.
std::string_view local(std::string_view name) {
    const std::size_t at = name.rfind(':');

    return at == std::string_view::npos ? name : name.substr(at + 1);
}

// Appends to `into` every element named `name`, any prefix, in `one` and below.
void gathered(const xml::Node& one, std::string_view name, std::vector<const xml::Node*>& into) {
    if (one.kind != xml::Kind::element) return;

    if (local(one.name) == name) into.push_back(&one);

    for (const xml::Node& each : one.children) gathered(each, name, into);
}

// Every element named `name` in `document`, in its order.
std::vector<const xml::Node*> found(const xml::Document& document, std::string_view name) {
    std::vector<const xml::Node*> made;

    for (const xml::Node& one : document.nodes) gathered(one, name, made);

    return made;
}

// The first child element of `one` named `name`, any prefix, or null.
const xml::Node* inner(const xml::Node& one, std::string_view name) {
    for (const xml::Node& each : one.children)
        if (each.kind == xml::Kind::element && local(each.name) == name) return &each;

    return nullptr;
}

// The text of the first element named `name` in `document`, trimmed; empty without one.
std::string said_in(const xml::Document& document, std::string_view name) {
    const std::vector<const xml::Node*> all = found(document, name);

    return all.empty() ? std::string{} : std::string{trimmed(xml::text(*all.front()))};
}

// The service of the description `document`, which came from `from`: the best one that opens ports,
// its control address resolved against URLBase when the description has it, otherwise against
// `from`; never over HTTPS, which a router does not speak.
std::optional<std::pair<Url, std::string>> controlled(const xml::Document& document, const Url& from) {
    Url base = from;

    if (const std::string told = said_in(document, "URLBase"); !told.empty())
        if (const std::optional<Url> made = Url::parsed(told)) base = *made;

    const std::vector<const xml::Node*> all = found(document, "service");

    for (const std::string_view wanted : services) {
        for (const xml::Node* one : all) {
            const xml::Node* type = inner(*one, "serviceType");
            const xml::Node* control = inner(*one, "controlURL");

            if (type == nullptr || control == nullptr || trimmed(xml::text(*type)) != wanted) continue;

            const std::optional<Url> url = base.resolved(trimmed(xml::text(*control)));

            if (!url || url->secure) continue;

            return std::pair{*url, std::string{wanted}};
        }
    }

    return std::nullopt;
}

// `text` with the five special characters of XML escaped.
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

// The arguments of a SOAP action, in their order.
using Arguments = std::vector<std::pair<std::string, std::string>>;

// The arguments of AddPortMapping: `inside` of `mine` as `outside` over `protocol`, described by
// `label`, for `lease` seconds.
Arguments mapping(std::uint16_t outside, const std::string& protocol, std::uint16_t inside, const Address& mine, const std::string& label, std::uint32_t lease) {
    Arguments made;

    made.emplace_back("NewRemoteHost", "");
    made.emplace_back("NewExternalPort", std::to_string(outside));
    made.emplace_back("NewProtocol", protocol);
    made.emplace_back("NewInternalPort", std::to_string(inside));
    made.emplace_back("NewInternalClient", mine.text());
    made.emplace_back("NewEnabled", "1");
    made.emplace_back("NewPortMappingDescription", label);
    made.emplace_back("NewLeaseDuration", std::to_string(lease));

    return made;
}

// The arguments of DeletePortMapping for the opening `one`.
Arguments unmapping(const Opening& one) {
    Arguments made;

    made.emplace_back("NewRemoteHost", "");
    made.emplace_back("NewExternalPort", std::to_string(one.outside.port()));
    made.emplace_back("NewProtocol", one.carrier == Carrier::tcp ? "TCP" : "UDP");

    return made;
}

// The plain HTTP of the home network: no proxy, no TLS, small answers.
Http homely() {
    Limits most;

    most.body = upnp_body;

    return Http{Optional<Tls>{}, most, Proxy{}};
}

// The answer of the action `action` of the service `control` to `arguments`, before `until`. A
// refusal is HTTP 500 with UPnPError, whose number goes into the `Trouble` (refused; denied for
// 606, an action not allowed), since the caller decides about 718 and 725 itself.
Task<xml::Document> soap(std::pair<Url, std::string> control, std::string action, Arguments arguments,
                         std::chrono::steady_clock::time_point until) {
    std::string body = "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                       "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" +
                       action + " xmlns:u=\"" + control.second + "\">";

    for (const auto& [name, value] : arguments) body += "<" + name + ">" + escaped(value) + "</" + name + ">";

    body += "</u:" + action + "></s:Body></s:Envelope>\r\n";

    Request asked;

    asked.method = "POST";
    asked.url = control.first;
    asked.headers.push_back(Header{"Content-Type", "text/xml; charset=\"utf-8\""});
    asked.headers.push_back(Header{"SOAPAction", "\"" + control.second + "#" + action + "\""});
    asked.body = bytes(body);

    const Response got = co_await within(left(until), homely().fetch(std::move(asked)));

    if (got.status != 200 && got.status != 500) throw Trouble{Kind::refused, "The router answered UPnP with HTTP " + std::to_string(got.status) + "."};

    xml::Document made = xml::parsed(text::of(got.body), upnp_xml);

    if (got.status == 200) co_return made;

    const std::string code = said_in(made, "errorCode");
    const std::string description = said_in(made, "errorDescription");
    int number = 0;

    if (!code.empty() && code.size() <= 6 && std::all_of(code.begin(), code.end(), [](char one) { return one >= '0' && one <= '9'; })) number = std::stoi(code);

    throw Trouble{number == 606 ? Kind::denied : Kind::refused, "The router refused the port (UPnP: " + (description.empty() ? "error " + code : description) + ").",
                  {}, number};
}

// The UPnP service: the one found before, or the first that a description found by SSDP offers,
// then remembered; nothing when there is none before `until`.
Task<std::optional<std::pair<Url, std::string>>> found_control(Kept held, std::chrono::steady_clock::time_point until) {
    {
        const std::lock_guard<std::mutex> kept{held->guard};

        if (held->control) co_return held->control;
    }

    for (const Url& one : co_await discovered(held, until)) {
        if (std::chrono::steady_clock::now() >= until) break;

        std::optional<std::pair<Url, std::string>> made;

        try {
            Request asked;

            asked.url = one;

            const Response got = co_await within(left(until), homely().fetch(std::move(asked)));

            if (got.status == 200) made = controlled(xml::parsed(text::of(got.body), upnp_xml), got.url);
        } catch (const Trouble&) {
        }

        if (made) {
            const std::lock_guard<std::mutex> kept{held->guard};

            held->control = made;

            co_return made;
        }
    }

    co_return std::nullopt;
}

// `inside` of `mine` opened through UPnP IGD with AddPortMapping, then the question for the
// external address; nothing without a service before `until`. Throws `Trouble` when the router
// refuses.
Task<std::optional<Opening>> upnp(Kept held, Address mine, Carrier carrier, std::uint16_t inside, std::uint32_t lifetime,
                                  std::chrono::steady_clock::time_point until) {
    const std::optional<std::pair<Url, std::string>> control = co_await found_control(held, until);

    if (!control) co_return std::nullopt;

    const std::string protocol = carrier == Carrier::tcp ? "TCP" : "UDP";
    std::uint32_t lease = std::min(lifetime, upnp_longest);
    std::uint16_t outside = inside;

    for (int turn = 0;; ++turn) {
        std::optional<Trouble> refused;

        try {
            (void)co_await soap(*control, "AddPortMapping", mapping(outside, protocol, inside, mine, held->routing.label, lease), until);

            break;
        } catch (const Trouble& why) {
            refused = why;
        }

        // 725: the router knows only lasting mappings. 718: the external port is taken, so we draw
        // another one.
        if (refused->code() == 725 && lease != 0 && turn < 6) {
            lease = 0;

            continue;
        }

        if (refused->code() == 718 && turn < 6) {
            const std::uint16_t taken = outside;

            while (outside == taken) {
                const Bytes drawn = crypto::random(2);

                outside = static_cast<std::uint16_t>(1024 + ((std::to_integer<unsigned>(drawn[0]) << 8 | std::to_integer<unsigned>(drawn[1])) % (65536 - 1024)));
            }

            continue;
        }

        throw *refused;
    }

    Address seen{Family::four, {}, outside};

    try {
        const xml::Document said = co_await soap(*control, "GetExternalIPAddress", Arguments{}, until);
        const std::string told = said_in(said, "NewExternalIPAddress");

        if (!told.empty()) seen = Address{told, outside};
    } catch (const Trouble&) {
    }

    co_return Opening{Opener::upnp, carrier, inside, seen, std::chrono::seconds{lease}};
}

// Our address in the home network: the one given, or the one that leads to the router.
Task<Address> ours(Kept held) {
    if (held->routing.mine.held()) co_return held->routing.mine;

    co_return co_await leaving(held->routing.gateway);
}

// `inside` opened for `lasting`: the way that worked last time first, then PCP, NAT-PMP when the
// router knows only that, and UPnP; a router that spoke NAT-PMP before is not asked PCP. The first
// refusal comes out, or unsupported when nothing answered.
Task<Opening> opening(Kept held, Carrier carrier, std::uint16_t inside, std::chrono::seconds lasting) {
    const auto until = std::chrono::steady_clock::now() + held->routing.patience;
    const Address mine = co_await ours(held);
    const auto lifetime = static_cast<std::uint32_t>(std::clamp<std::chrono::seconds::rep>(lasting.count(), 1, 0x7fffffff));
    std::optional<Opener> worked;
    std::exception_ptr first;
    std::optional<Opening> made;

    {
        const std::lock_guard<std::mutex> kept{held->guard};

        worked = held->worked;
    }

    if (worked == Opener::upnp) {
        try {
            made = co_await upnp(held, mine, carrier, inside, lifetime, until);
        } catch (const Trouble&) {
            first = std::current_exception();
        }
    }

    bool old = worked == Opener::pmp;

    if (!made && worked != Opener::pmp) {
        try {
            const PcpOutcome got = co_await pcp(held, mine, carrier, inside, lifetime);

            made = got.opened;
            old = got.old;
        } catch (const Trouble&) {
            if (!first) first = std::current_exception();
        }
    }

    if (!made && old) {
        try {
            made = co_await pmp(held, carrier, inside, lifetime);
        } catch (const Trouble&) {
            if (!first) first = std::current_exception();
        }
    }

    if (!made && worked != Opener::upnp) {
        try {
            made = co_await upnp(held, mine, carrier, inside, lifetime, until);
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

    throw Trouble{Kind::unsupported, "No router answered PCP, NAT-PMP or UPnP."};
}

// The mapping `one` closed the way it was opened. Throws `Trouble` (missing) when this router did
// not open it, overdue when the router does not answer, and the router's refusal of UPnP.
Task<void> closing(Kept held, Opening one) {
    if (one.by == Opener::pmp) {
        if (!co_await pmp(held, one.carrier, one.inside, 0)) throw Trouble{Kind::overdue, "The router did not answer the closing."};

        co_return;
    }

    if (one.by == Opener::upnp) {
        std::optional<std::pair<Url, std::string>> control;

        {
            const std::lock_guard<std::mutex> kept{held->guard};

            control = held->control;
        }

        if (!control) throw Trouble{Kind::missing, "This router did not open that port."};

        (void)co_await soap(*control, "DeletePortMapping", unmapping(one), std::chrono::steady_clock::now() + held->routing.patience);

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
