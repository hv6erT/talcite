#include "tal/testing.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "talcite/address.hpp"
#include "talcite/udp.hpp"
#include "talcite/stunclient.hpp"
#include "talcite/time.hpp"

using namespace talcite;

// The fake STUN server puts its responses together byte by byte, not with our codec. That makes
// this a real test of conformance with the format, not a check of the code against itself.
namespace {

const net::Patience soon{6000};

// Appends a 16-bit value in network byte order.
void pushed(Bytes& into, std::uint16_t what) {
    into.push_back(static_cast<std::byte>(what >> 8));
    into.push_back(static_cast<std::byte>(what & 0xFF));
}

// A STUN Binding success response with transaction number `number` and one IPv4 address
// attribute of type `kind` for `one`:`port`, XOR-ed with the magic cookie when `xored` is set.
Bytes addressed(std::uint16_t kind, const std::string& one, std::uint16_t port,
                const Bytes& number, bool xored) {
    Bytes value;

    value.push_back(std::byte{0});
    value.push_back(std::byte{1});

    pushed(value, xored ? static_cast<std::uint16_t>(port ^ 0x2112) : port);

    std::uint32_t raw = 0;

    std::size_t at = 0;

    for (int which = 0; which < 4; ++which) {
        std::string part;

        while (at < one.size() && one[at] != '.') part += one[at++];

        ++at;

        raw = (raw << 8) | static_cast<std::uint32_t>(std::stoul(part));
    }

    const std::uint32_t cookie = 0x2112A442u;

    if (xored) raw ^= cookie;

    for (int which = 0; which < 4; ++which)
        value.push_back(static_cast<std::byte>((raw >> (24 - 8 * which)) & 0xFF));

    Bytes made;

    pushed(made, 0x0101);
    pushed(made, static_cast<std::uint16_t>(4 + value.size()));
    pushed(made, 0x2112);
    pushed(made, 0xA442);

    for (const std::byte what : number) made.push_back(what);

    pushed(made, kind);
    pushed(made, static_cast<std::uint16_t>(value.size()));

    for (const std::byte what : value) made.push_back(what);

    return made;
}

// A Binding success response with transaction number `number` and an XOR-MAPPED-ADDRESS of
// 2001:db8::1 on `port`.
Bytes sixed(const Bytes& number, std::uint16_t port) {
    Bytes value;

    value.push_back(std::byte{0});
    value.push_back(std::byte{2});

    pushed(value, static_cast<std::uint16_t>(port ^ 0x2112));

    // 2001:db8::1 XOR-ed with the cookie and the transaction number.
    const std::uint8_t plain[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
    const std::uint8_t cookie[4] = {0x21, 0x12, 0xA4, 0x42};

    for (std::size_t at = 0; at < 16; ++at) {
        const std::byte mask =
            at < 4 ? static_cast<std::byte>(cookie[at]) : number[at - 4];

        value.push_back(static_cast<std::byte>(plain[at]) ^ mask);
    }

    Bytes made;

    pushed(made, 0x0101);
    pushed(made, static_cast<std::uint16_t>(4 + value.size()));
    pushed(made, 0x2112);
    pushed(made, 0xA442);

    for (const std::byte what : number) made.push_back(what);

    pushed(made, 0x0020);
    pushed(made, static_cast<std::uint16_t>(value.size()));

    for (const std::byte what : value) made.push_back(what);

    return made;
}

// A Binding error response with transaction number `number` and error code 420.
Bytes refused(const Bytes& number) {
    Bytes value;

    value.push_back(std::byte{0});
    value.push_back(std::byte{0});
    value.push_back(std::byte{4});
    value.push_back(std::byte{20});

    for (const char one : std::string{"Nie ma"}) value.push_back(static_cast<std::byte>(one));

    while (value.size() % 4 != 0) value.push_back(std::byte{0});

    Bytes made;

    pushed(made, 0x0111);
    pushed(made, static_cast<std::uint16_t>(4 + value.size()));
    pushed(made, 0x2112);
    pushed(made, 0xA442);

    for (const std::byte what : number) made.push_back(what);

    pushed(made, 0x0009);
    pushed(made, static_cast<std::uint16_t>(value.size()));

    for (const std::byte what : value) made.push_back(what);

    return made;
}

// The 12-byte transaction number of a STUN message.
Bytes numbered(const Bytes& what) {
    return Bytes{what.begin() + 8, what.begin() + 20};
}

// A UDP socket on an IPv4 loopback address with a port chosen by the system.
net::Udp standing() { return net::Udp{net::Address::loopback(net::Family::four, 0)}; }

using Server = std::pair<std::string, std::uint16_t>;

// Public STUN servers built into the test, so that conformance with the real world can be checked
// with one command. The order is not accidental: `mapping()` asks the first two, and those must
// belong to different operators. Both Google names currently resolve to the same address, so next
// to each other they would give the verdict "alike" from one server, which is untrue.
std::vector<Server> known() {
    return {{"stun.l.google.com", 19302},
            {"stun.cloudflare.com", 3478},
            {"stun1.l.google.com", 19302},
            {"stun.nextcloud.com", 443}};
}

}

TRIAL("Scout: the reflected address from the XOR-ed attribute") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::StunClient scout{mine, soon};

    scout.add(server.mine());

    auto asking = scout.seen();

    const net::Datagram came = server.receive(1024, soon).get();

    server.send(came.from, addressed(0x0020, "203.0.113.7", 40404, numbered(came.bytes), true),
                soon)
        .get();

    const net::Address made = std::move(asking).get();

    SAME(made.text(), std::string{"203.0.113.7"});
    SAME(made.port(), static_cast<std::uint16_t>(40404));
    SAME(scout.last().text(), std::string{"203.0.113.7"});
}

TRIAL("Scout: the old attribute without XOR is understood too") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::StunClient scout{mine, soon};

    scout.add(server.mine());

    auto asking = scout.seen();

    const net::Datagram came = server.receive(1024, soon).get();

    server.send(came.from, addressed(0x0001, "198.51.100.9", 1234, numbered(came.bytes), false),
                soon)
        .get();

    SAME(std::move(asking).get().text(), std::string{"198.51.100.9"});
}

TRIAL("Scout: an address of the IPv6 family") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::StunClient scout{mine, soon};

    scout.add(server.mine());

    auto asking = scout.seen();

    const net::Datagram came = server.receive(1024, soon).get();

    server.send(came.from, sixed(numbered(came.bytes), 9999), soon).get();

    const net::Address made = std::move(asking).get();

    SAME(made.family(), net::Family::six);
    SAME(made.text(), std::string{"2001:db8::1"});
    SAME(made.port(), static_cast<std::uint16_t>(9999));
}

TRIAL("Scout: someone else's response is not accepted") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::StunClient scout{mine, soon};

    scout.add(server.mine());

    auto asking = scout.seen();

    const net::Datagram came = server.receive(1024, soon).get();

    Bytes other = numbered(came.bytes);

    other[0] = ~other[0];

    // First a response with another transaction number, then the right one.
    server.send(came.from, addressed(0x0020, "1.2.3.4", 1, other, true), soon).get();
    server
        .send(came.from, addressed(0x0020, "203.0.113.7", 40404, numbered(came.bytes), true), soon)
        .get();

    SAME(std::move(asking).get().text(), std::string{"203.0.113.7"});
}

TRIAL("Scout: garbage on the socket does not get in the way") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::StunClient scout{mine, soon};

    scout.add(server.mine());

    auto asking = scout.seen();

    const net::Datagram came = server.receive(1024, soon).get();

    Bytes trash(40, std::byte{0xFF});

    server.send(came.from, trash, soon).get();
    server
        .send(came.from, addressed(0x0020, "203.0.113.7", 40404, numbered(came.bytes), true), soon)
        .get();

    SAME(std::move(asking).get().text(), std::string{"203.0.113.7"});
}

TRIAL("Scout: the server's refusal comes out as trouble") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::StunClient scout{mine, soon};

    scout.add(server.mine());

    auto asking = scout.seen();

    const net::Datagram came = server.receive(1024, soon).get();

    server.send(came.from, refused(numbered(came.bytes)), soon).get();

    bool amiss = false;

    try {
        (void)std::move(asking).get();
    } catch (const net::Trouble& why) {
        amiss = true;

        SAME(why.code(), 420);
    }

    OUGHT(amiss);
}

TRIAL("Scout: a retry after a lost request") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::StunClient scout{mine, net::Patience{5000}};

    scout.add(server.mine());

    auto asking = scout.seen();

    // The first request is lost; we answer only the second.
    (void)server.receive(1024, soon).get();

    const net::Datagram came = server.receive(1024, soon).get();

    server.send(came.from, addressed(0x0020, "203.0.113.7", 40404, numbered(came.bytes), true),
                soon)
        .get();

    SAME(std::move(asking).get().text(), std::string{"203.0.113.7"});
}

TRIAL("Scout: silence ends with the deadline") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::StunClient scout{mine, net::Patience{300}};

    scout.add(server.mine());

    bool late = false;

    try {
        (void)scout.seen().get();
    } catch (const Overdue&) {
        late = true;
    }

    OUGHT(late);
}

TRIAL("Scout: without servers there is nothing to ask") {
    net::Udp mine = standing();

    net::StunClient scout{mine, soon};

    bool amiss = false;

    try {
        (void)scout.seen().get();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    OUGHT(amiss);
}

TRIAL("Scout: two questions at once are refused") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::StunClient scout{mine, net::Patience{400}};

    scout.add(server.mine());

    auto first = scout.seen();

    bool busy = false;

    try {
        (void)scout.seen().get();
    } catch (const net::Trouble&) {
        busy = true;
    }

    OUGHT(busy);

    try {
        (void)std::move(first).get();
    } catch (const std::exception&) {
    }
}

TRIAL("Scout: matching reflections mean hole punching makes sense") {
    net::Udp mine = standing();
    net::Udp one = standing();
    net::Udp two = standing();

    net::StunClient scout{mine, soon};

    scout.add(one.mine());
    scout.add(two.mine());

    auto asking = scout.mapping();

    const net::Datagram first = one.receive(1024, soon).get();

    one.send(first.from, addressed(0x0020, "203.0.113.7", 40404, numbered(first.bytes), true),
             soon)
        .get();

    const net::Datagram second = two.receive(1024, soon).get();

    two.send(second.from, addressed(0x0020, "203.0.113.7", 40404, numbered(second.bytes), true),
             soon)
        .get();

    SAME(std::move(asking).get(), net::StunClient::Mapping::alike);
    SAME(scout.known(), net::StunClient::Mapping::alike);
}

TRIAL("Scout: differing reflections mean a symmetric NAT") {
    net::Udp mine = standing();
    net::Udp one = standing();
    net::Udp two = standing();

    net::StunClient scout{mine, soon};

    scout.add(one.mine());
    scout.add(two.mine());

    auto asking = scout.mapping();

    const net::Datagram first = one.receive(1024, soon).get();

    one.send(first.from, addressed(0x0020, "203.0.113.7", 40404, numbered(first.bytes), true),
             soon)
        .get();

    const net::Datagram second = two.receive(1024, soon).get();

    two.send(second.from, addressed(0x0020, "203.0.113.7", 51515, numbered(second.bytes), true),
             soon)
        .get();

    SAME(std::move(asking).get(), net::StunClient::Mapping::symmetric);
}

TRIAL("Scout: with one server there is no verdict") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::StunClient scout{mine, soon};

    scout.add(server.mine());

    SAME(scout.mapping().get(), net::StunClient::Mapping::unknown);
}

// The only trial in the whole suite that goes out to the internet. No network is not a failure
// here: the suite must pass on a machine that is cut off, and being cut off cannot be told apart
// from a bug in the code. So the trial prints what it found and fails only when a server answered
// wrongly: with an empty address or a verdict without content. The reading from the real world
// has to be read with one's own eyes, most conveniently like this:
//
//     talcite-test --only "Scout: real servers on the internet"
TRIAL("Scout: real servers on the internet") {
    net::Udp mine{net::Address::any(net::Family::four, 0)};

    net::StunClient scout{mine, net::Patience{4000}};

    for (const Server& one : known()) {
        try {
            const std::vector<net::Address> found =
                net::resolve(one.first, one.second, net::Family::four).get();

            if (!found.empty()) scout.add(found.front());
        } catch (const std::exception&) {
        }
    }

    if (scout.servers().empty()) {
        std::cout << "        no network: the server names could not be resolved, trial skipped\n";

        return;
    }

    net::Address seen;

    try {
        seen = scout.seen().get();
    } catch (const std::exception& why) {
        std::cout << "        no answer from the network (" << why.what() << "), trial skipped\n";

        return;
    }

    std::cout << "        they see me as " << seen.spelled() << "\n";

    OUGHT(seen.held());
    OUGHT(seen.port() != 0);

    const net::StunClient::Mapping what = scout.mapping().get();

    std::cout << "        mapping: "
              << (what == net::StunClient::Mapping::alike
                      ? "alike"
                      : what == net::StunClient::Mapping::symmetric ? "symmetric" : "unknown")
              << "\n";

    // The verdict is printed, not checked. "Unknown" here means that one of the first two servers
    // went silent, and that is the state of the network, not a bug in the code, and the test must
    // not fail over it. The real check of conformance is the address above: a foreign server
    // answered so that our codec got a sensible address and port out of it.
}

// The "already asking" flag goes away at the latest together with the promise, so a question
// cancelled, or abandoned untouched, does not freeze the scout for good. Cancelling does not stop a
// question that is already running (that is the contract of all of talcite), so the scout gets
// its voice back only when that one ends.
TRIAL("Scout: a cancelled question does not freeze the scout for good") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::StunClient scout{mine, net::Patience{300}};

    scout.add(server.mine());

    {
        auto first = scout.seen();

        first.cancel();
    }

    bool again = false;

    for (int turn = 0; turn < 40 && !again; ++turn) {
        try {
            (void)scout.seen().get();
        } catch (const Overdue&) {
            again = true;
        } catch (const net::Trouble&) {
            later(std::chrono::milliseconds{100}).get();
        }
    }

    OUGHT(again);
}

// The same socket later carries the peer's traffic, so one ordinary large packet must not end the
// scouting.
TRIAL("Scout: an oversized datagram does not break the question") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::StunClient scout{mine, soon};

    scout.add(server.mine());

    auto asking = scout.seen();

    const net::Datagram came = server.receive(1024, soon).get();

    server.send(came.from, Bytes(4000, std::byte{0x41}), soon).get();

    server.send(came.from, addressed(0x0020, "203.0.113.7", 40404, numbered(came.bytes), true),
                soon)
        .get();

    SAME(std::move(asking).get().text(), std::string{"203.0.113.7"});
}

// One misconfigured server at the head of the list must not rule out all the ones after it.
TRIAL("Scout: one server's refusal does not end the round") {
    net::Udp mine = standing();
    net::Udp bad = standing();
    net::Udp good = standing();

    net::StunClient scout{mine, soon};

    scout.add(bad.mine());
    scout.add(good.mine());

    auto asking = scout.seen();

    const net::Datagram first = bad.receive(1024, soon).get();

    bad.send(first.from, refused(numbered(first.bytes)), soon).get();

    const net::Datagram second = good.receive(1024, soon).get();

    good.send(second.from, addressed(0x0020, "198.51.100.9", 1234, numbered(second.bytes), true),
              soon)
        .get();

    SAME(std::move(asking).get().text(), std::string{"198.51.100.9"});
}

// The same address added twice must not give two identical answers and from them the verdict
// "alike", which would be a lie in the dangerous direction.
TRIAL("Scout: the same server twice gives no verdict") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::StunClient scout{mine, net::Patience{600}};

    scout.add(server.mine());
    scout.add(server.mine());

    SAME(scout.mapping().get(), net::StunClient::Mapping::unknown);
}

// A broken XOR-ed attribute must not erase a correctly read old one.
TRIAL("Scout: a bad attribute does not erase a good one") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::StunClient scout{mine, soon};

    scout.add(server.mine());

    auto asking = scout.seen();

    const net::Datagram came = server.receive(1024, soon).get();

    Bytes made = addressed(0x0001, "198.51.100.9", 1234, numbered(came.bytes), false);

    // We append an XOR-ed attribute with an impossible family and fix the message length.
    Bytes value;

    value.push_back(std::byte{0});
    value.push_back(std::byte{7});
    pushed(value, 0);
    pushed(value, 0);
    pushed(value, 0);

    pushed(made, 0x0020);
    pushed(made, static_cast<std::uint16_t>(value.size()));

    for (const std::byte one : value) made.push_back(one);

    const std::uint16_t much = static_cast<std::uint16_t>(made.size() - 20);

    made[2] = static_cast<std::byte>(much >> 8);
    made[3] = static_cast<std::byte>(much & 0xFF);

    server.send(came.from, made, soon).get();

    SAME(std::move(asking).get().text(), std::string{"198.51.100.9"});
}

// The reason for a refusal is written by a stranger, and the text goes to the terminal.
TRIAL("Scout: the refusal reason carries no control characters") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::StunClient scout{mine, soon};

    scout.add(server.mine());

    auto asking = scout.seen();

    const net::Datagram came = server.receive(1024, soon).get();

    Bytes value;

    value.push_back(std::byte{0});
    value.push_back(std::byte{0});
    value.push_back(std::byte{4});
    value.push_back(std::byte{20});

    for (const char one : std::string{"\x1b[2J\x07 nic"}) value.push_back(static_cast<std::byte>(one));

    while (value.size() % 4 != 0) value.push_back(std::byte{0});

    Bytes made;

    pushed(made, 0x0111);
    pushed(made, static_cast<std::uint16_t>(4 + value.size()));
    pushed(made, 0x2112);
    pushed(made, 0xA442);

    for (const std::byte one : numbered(came.bytes)) made.push_back(one);

    pushed(made, 0x0009);
    pushed(made, static_cast<std::uint16_t>(value.size()));

    for (const std::byte one : value) made.push_back(one);

    server.send(came.from, made, soon).get();

    bool amiss = false;

    try {
        (void)std::move(asking).get();
    } catch (const net::Trouble& why) {
        amiss = true;

        const std::string said{why.what()};

        for (const char one : said) OUGHT(one >= 0x20 || one < 0);
    }

    OUGHT(amiss);
}
