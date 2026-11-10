#include "tal/testing.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "talcite/address.hpp"
#include "talcite/digest.hpp"
#include "talcite/peer.hpp"
#include "talcite/udp.hpp"
#include "talcite/relay.hpp"
#include "talcite/time.hpp"

#include "stunning.hpp"

using namespace talcite;

// The fake relay puts its responses together byte by byte, not with our codec; otherwise the code
// would be checked against itself. A real TURN server requires authentication, and that requires
// digests that this sandbox does not provide; the one here allocates without credentials, so all
// the rest of the conversation (allocation, permission, sending, receiving, refreshing) is checked
// everywhere.
namespace {

const net::Patience soon{4000};

// Appends a 16-bit value in network byte order.
void pushed(Bytes& into, std::uint16_t what) {
    into.push_back(static_cast<std::byte>(what >> 8));
    into.push_back(static_cast<std::byte>(what & 0xFF));
}

// Writes the message length (everything after the 20-byte header) into the STUN header.
void minded(Bytes& into) {
    const std::uint16_t much = static_cast<std::uint16_t>(into.size() - 20);

    into[2] = static_cast<std::byte>(much >> 8);
    into[3] = static_cast<std::byte>(much & 0xFF);
}

// A STUN header of type `kind` with the transaction number copied from the request `what`; the
// length is filled in as attributes are added.
Bytes headed(std::uint16_t kind, const Bytes& what) {
    Bytes made;

    pushed(made, kind);
    pushed(made, 0);
    pushed(made, 0x2112);
    pushed(made, 0xA442);

    // The transaction number is copied from the request; by it, and only by it, the client
    // recognizes the response.
    for (std::size_t at = 8; at < 20; ++at) made.push_back(what[at]);

    return made;
}

// Appends an XOR-ed IPv4 address attribute of type `kind` for `one`:`port`.
void addressed(Bytes& made, std::uint16_t kind, const std::string& one, std::uint16_t port) {
    Bytes value;

    value.push_back(std::byte{0});
    value.push_back(std::byte{1});

    pushed(value, static_cast<std::uint16_t>(port ^ 0x2112));

    std::uint32_t raw = 0;

    std::size_t at = 0;

    for (int which = 0; which < 4; ++which) {
        std::string part;

        while (at < one.size() && one[at] != '.') part += one[at++];

        ++at;

        raw = (raw << 8) | static_cast<std::uint32_t>(std::stoul(part));
    }

    raw ^= 0x2112A442u;

    for (int which = 0; which < 4; ++which)
        value.push_back(static_cast<std::byte>((raw >> (24 - 8 * which)) & 0xFF));

    pushed(made, kind);
    pushed(made, static_cast<std::uint16_t>(value.size()));

    for (const std::byte one : value) made.push_back(one);

    minded(made);
}

// Appends a 32-bit attribute of type `kind`.
void numbered(Bytes& made, std::uint16_t kind, std::uint32_t what) {
    pushed(made, kind);
    pushed(made, 4);

    pushed(made, static_cast<std::uint16_t>(what >> 16));
    pushed(made, static_cast<std::uint16_t>(what & 0xFFFF));

    minded(made);
}

// Appends a DATA attribute with the given bytes, padded to a multiple of four.
void bearing(Bytes& made, const Bytes& what) {
    pushed(made, 0x0013);
    pushed(made, static_cast<std::uint16_t>(what.size()));

    for (const std::byte one : what) made.push_back(one);

    while (made.size() % 4 != 0) made.push_back(std::byte{0});

    minded(made);
}


// Appends a DATA attribute with the given text.
void bearing(Bytes& made, const std::string& what) { bearing(made, bytes(what)); }

// A UDP socket on an IPv4 loopback address with a port chosen by the system.
net::Udp standing() { return net::Udp{net::Address::loopback(net::Family::four, 0)}; }

// A Binding request and response put together byte by byte, because they play the other side. If
// our codec made them, the code would be checked against itself, and the point here is whether the
// peer understands a stranger.
Bytes knocking(const net::Number& number) {
    Bytes made;

    pushed(made, 0x0001);
    pushed(made, 0);
    pushed(made, 0x2112);
    pushed(made, 0xA442);

    for (const std::byte one : number) made.push_back(one);

    return made;
}

// A Binding success response with the given transaction number and no attributes.
Bytes answering(const net::Number& number) {
    Bytes made;

    pushed(made, 0x0101);
    pushed(made, 0);
    pushed(made, 0x2112);
    pushed(made, 0xA442);

    for (const std::byte one : number) made.push_back(one);

    return made;
}

// A Data indication with content from the peer, which is how the server hands the client someone
// else's bytes.
Bytes carrying(const net::Address& from, const Bytes& what) {
    Bytes made;

    pushed(made, 0x0017);
    pushed(made, 0);
    pushed(made, 0x2112);
    pushed(made, 0xA442);

    for (int which = 0; which < 12; ++which) made.push_back(static_cast<std::byte>(which + 1));

    addressed(made, 0x0012, from.text(), from.port());
    bearing(made, what);

    return made;
}

// The channel number from a ChannelBind request, picked out of the attributes by hand, because
// `Message` does not carry it.
std::uint16_t channelled(const Bytes& what) {
    std::size_t at = 20;

    while (at + 4 <= what.size()) {
        const std::uint16_t kind = static_cast<std::uint16_t>(
            (std::to_integer<unsigned>(what[at]) << 8) | std::to_integer<unsigned>(what[at + 1]));

        const std::uint16_t wide = static_cast<std::uint16_t>(
            (std::to_integer<unsigned>(what[at + 2]) << 8) |
            std::to_integer<unsigned>(what[at + 3]));

        if (kind == 0x000C && at + 6 <= what.size())
            return static_cast<std::uint16_t>((std::to_integer<unsigned>(what[at + 4]) << 8) |
                                              std::to_integer<unsigned>(what[at + 5]));

        at += 4u + wide + ((4u - wide % 4u) % 4u);
    }

    return 0;
}

// Content on a channel, put together byte by byte: number, length, content. No STUN header.
Bytes channelling(std::uint16_t channel, const std::string& what) {
    Bytes made;

    pushed(made, channel);
    pushed(made, static_cast<std::uint16_t>(what.size()));

    for (const char one : what) made.push_back(static_cast<std::byte>(one));

    return made;
}

// An allocation on the fake server: it takes the request and hands out the lent address. It also
// returns the address of the client's socket, because everything after goes there.
net::Address allotted(net::Udp& server, const net::Patience& patience) {
    const net::Datagram came = server.receive(1024, patience).get();

    Bytes back = headed(0x0103, came.bytes);

    addressed(back, 0x0016, "203.0.113.5", 49152);
    numbered(back, 0x000D, 600);

    server.send(came.from, back, patience).get();

    return came.from;
}

// A relay with an open allocation and a bound channel to the given peer. The channel is requested
// by itself on the first send, so here we only confirm it and remember the number.
struct Ready {
    net::Address whence;
    std::uint16_t channel = 0;
};

// Opens an allocation on the fake server, sends once to `them`, and confirms the ChannelBind that
// the send triggers; returns the client's address and the channel number.
Ready readied(net::Relay& relay, net::Udp& server, const net::Address& them) {
    Ready made;

    auto asking = relay.open(soon);

    made.whence = allotted(server, soon);

    (void)std::move(asking).get();

    relay.send(them, bytes("raz"), soon).get();

    for (const auto latest = std::chrono::steady_clock::now() + std::chrono::seconds{20};
         made.channel == 0 && std::chrono::steady_clock::now() < latest;) {
        const net::Datagram came = server.receive(1024, soon).get();

        net::Message told;

        if (!net::understood(came.bytes, told)) continue;

        if (told.task != net::Task::channel || told.sort != net::Sort::asking) continue;

        // A channel binds to the address together with the port, unlike a permission.
        SAME(told.peer, them);

        made.channel = channelled(came.bytes);

        server.send(made.whence, headed(0x0109, came.bytes), soon).get();
    }

    return made;
}

}

TRIAL("Relay: the allocation gives back the lent address") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::Relay relay{mine, server.mine()};

    auto asking = relay.open(soon);

    const net::Datagram came = server.receive(1024, soon).get();

    // The request must be an allocation and must ask for UDP.
    net::Message told;

    OUGHT(net::understood(came.bytes, told));
    SAME(static_cast<int>(told.task), static_cast<int>(net::Task::allocate));
    SAME(static_cast<int>(told.sort), static_cast<int>(net::Sort::asking));

    Bytes back = headed(0x0103, came.bytes);

    addressed(back, 0x0016, "203.0.113.5", 49152);
    addressed(back, 0x0020, "198.51.100.9", 40404);
    numbered(back, 0x000D, 600);

    server.send(came.from, back, soon).get();

    const net::Address lent = std::move(asking).get();

    SAME(lent.text(), std::string{"203.0.113.5"});
    SAME(lent.port(), static_cast<std::uint16_t>(49152));
    SAME(relay.lent(), lent);
    SAME(relay.seen().text(), std::string{"198.51.100.9"});
}

TRIAL("Relay: a refused allocation comes out as trouble") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::Relay relay{mine, server.mine()};

    auto asking = relay.open(soon);

    const net::Datagram came = server.receive(1024, soon).get();

    Bytes back = headed(0x0113, came.bytes);

    Bytes value;

    value.push_back(std::byte{0});
    value.push_back(std::byte{0});
    value.push_back(std::byte{4});
    value.push_back(std::byte{86});

    for (const char one : std::string{"Za duzo"}) value.push_back(static_cast<std::byte>(one));

    while (value.size() % 4 != 0) value.push_back(std::byte{0});

    pushed(back, 0x0009);
    pushed(back, static_cast<std::uint16_t>(value.size()));

    for (const std::byte one : value) back.push_back(one);

    minded(back);

    server.send(came.from, back, soon).get();

    bool amiss = false;

    try {
        (void)std::move(asking).get();
    } catch (const net::Trouble& why) {
        amiss = true;

        SAME(why.code(), 486);
    }

    OUGHT(amiss);
}

// A server that demands credentials first answers with a refusal carrying a realm and a nonce.
// Without credentials there is nothing to compute the key from, and we must say so plainly instead
// of trying forever.
TRIAL("Relay: a demand for credentials without credentials ends plainly") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::Relay relay{mine, server.mine()};

    auto asking = relay.open(soon);

    const net::Datagram came = server.receive(1024, soon).get();

    Bytes back = headed(0x0113, came.bytes);

    Bytes value;

    value.push_back(std::byte{0});
    value.push_back(std::byte{0});
    value.push_back(std::byte{4});
    value.push_back(std::byte{1});

    pushed(back, 0x0009);
    pushed(back, static_cast<std::uint16_t>(value.size()));

    for (const std::byte one : value) back.push_back(one);

    minded(back);

    pushed(back, 0x0014);
    pushed(back, 4);

    for (const char one : std::string{"dom "}) back.push_back(static_cast<std::byte>(one));

    minded(back);

    pushed(back, 0x0015);
    pushed(back, 4);

    for (const char one : std::string{"raz "}) back.push_back(static_cast<std::byte>(one));

    minded(back);

    server.send(came.from, back, soon).get();

    bool amiss = false;

    try {
        (void)std::move(asking).get();
    } catch (const net::Trouble& why) {
        amiss = true;

        SAME(why.code(), 401);
    }

    OUGHT(amiss);
}

TRIAL("Relay: the server's silence ends with the deadline") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::Relay relay{mine, server.mine()};

    bool late = false;

    try {
        (void)relay.open(net::Patience{500}).get();
    } catch (const Overdue&) {
        late = true;
    }

    OUGHT(late);
}

TRIAL("Relay: a permission goes with the peer's address") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::Relay relay{mine, server.mine()};

    auto asking = relay.open(soon);

    net::Datagram came = server.receive(1024, soon).get();

    Bytes back = headed(0x0103, came.bytes);

    addressed(back, 0x0016, "203.0.113.5", 49152);
    numbered(back, 0x000D, 600);

    server.send(came.from, back, soon).get();

    (void)std::move(asking).get();

    auto second = relay.allow(net::Address{"198.51.100.77", 5555}, soon);

    came = server.receive(1024, soon).get();

    net::Message told;

    OUGHT(net::understood(came.bytes, told));
    SAME(static_cast<int>(told.task), static_cast<int>(net::Task::permission));
    SAME(told.peer.text(), std::string{"198.51.100.77"});
    SAME(told.peer.port(), static_cast<std::uint16_t>(5555));

    server.send(came.from, headed(0x0108, came.bytes), soon).get();

    std::move(second).get();
}

TRIAL("Relay: sending goes as an indication with content") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::Relay relay{mine, server.mine()};

    auto asking = relay.open(soon);

    net::Datagram came = server.receive(1024, soon).get();

    Bytes back = headed(0x0103, came.bytes);

    addressed(back, 0x0016, "203.0.113.5", 49152);
    numbered(back, 0x000D, 600);

    server.send(came.from, back, soon).get();

    (void)std::move(asking).get();

    relay.send(net::Address{"198.51.100.77", 5555}, bytes("przez przekaźnik"), soon).get();

    // The first send goes as an indication, and next to it goes the channel request; the order of
    // two datagrams is nobody's promise, so we look for the one that matters.
    net::Message told;

    bool sent = false;

    for (const auto latest = std::chrono::steady_clock::now() + std::chrono::seconds{20};
         !sent && std::chrono::steady_clock::now() < latest;) {
        came = server.receive(1024, soon).get();

        if (!net::understood(came.bytes, told)) continue;

        if (told.task != net::Task::sending) continue;

        SAME(static_cast<int>(told.sort), static_cast<int>(net::Sort::telling));
        SAME(told.peer.text(), std::string{"198.51.100.77"});
        SAME(text(told.data), std::string{"przez przekaźnik"});

        sent = true;
    }

    OUGHT(sent);
}

TRIAL("Relay: content from the peer comes with its address") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::Relay relay{mine, server.mine()};

    auto asking = relay.open(soon);

    const net::Datagram came = server.receive(1024, soon).get();

    Bytes back = headed(0x0103, came.bytes);

    addressed(back, 0x0016, "203.0.113.5", 49152);
    numbered(back, 0x000D, 600);

    server.send(came.from, back, soon).get();

    (void)std::move(asking).get();

    auto waiting = relay.receive(1024, soon);

    Bytes data = headed(0x0017, came.bytes);

    addressed(data, 0x0012, "198.51.100.77", 5555);
    bearing(data, "od obcego");

    server.send(came.from, data, soon).get();

    const net::Datagram given = std::move(waiting).get();

    SAME(given.from.text(), std::string{"198.51.100.77"});
    SAME(given.from.port(), static_cast<std::uint16_t>(5555));
    SAME(text(given.bytes), std::string{"od obcego"});
}

// A response comes only from the server. Someone else's datagram on this socket must neither pose
// as a response nor pass itself off as content from the peer.
TRIAL("Relay: a stranger's datagram does not pose as the relay") {
    net::Udp mine = standing();
    net::Udp server = standing();
    net::Udp evil = standing();

    net::Relay relay{mine, server.mine()};

    auto asking = relay.open(net::Patience{1500});

    const net::Datagram came = server.receive(1024, soon).get();

    // The stranger poses as the allocation, with a valid transaction number.
    Bytes back = headed(0x0103, came.bytes);

    addressed(back, 0x0016, "1.2.3.4", 1);
    numbered(back, 0x000D, 600);

    evil.send(mine.mine(), back, soon).get();

    bool late = false;

    try {
        (void)std::move(asking).get();
    } catch (const Overdue&) {
        late = true;
    }

    OUGHT(late);
    OUGHT(!relay.lent().held());
}

TRIAL("Relay: an empty one does nothing silently") {
    net::Relay relay;

    bool amiss = false;

    try {
        (void)relay.open(soon).get();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    OUGHT(amiss);
}

TRIAL("Relay: an oversized message is not lost on refusal") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::Relay relay{mine, server.mine()};

    auto asking = relay.open(soon);

    const net::Datagram came = server.receive(1024, soon).get();

    Bytes back = headed(0x0103, came.bytes);

    addressed(back, 0x0016, "203.0.113.5", 49152);
    numbered(back, 0x000D, 600);

    server.send(came.from, back, soon).get();

    (void)std::move(asking).get();

    Bytes data = headed(0x0017, came.bytes);

    addressed(data, 0x0012, "198.51.100.77", 5555);
    bearing(data, "dłuższe niż cztery bajty");

    server.send(came.from, data, soon).get();

    bool amiss = false;

    for (const auto latest = std::chrono::steady_clock::now() + std::chrono::seconds{20};
         !amiss && std::chrono::steady_clock::now() < latest;) {
        try {
            (void)relay.receive(4, net::Patience{100}).get();
        } catch (const Overdue&) {
            continue;
        } catch (const net::Trouble&) {
            amiss = true;
        }
    }

    OUGHT(amiss);

    SAME(text(relay.receive(1024, soon).get().bytes),
         std::string{"dłuższe niż cztery bajty"});
}

// A peer with a relay. There is no real NAT here, so the conversation is what gets checked:
// whether the lent address goes out to signaling, whether permissions go by address without the
// port, whether the response goes back on the channel the check came in on, and whether data then
// goes that way instead of directly.
TRIAL("Peer: the relay adds the lent path to its own candidates") {
    net::Udp server = standing();

    net::Peer left{standing(), net::Relay{standing(), server.mine()}};

    auto gathering = left.gather(net::StunClient{}, soon);

    (void)allotted(server, soon);

    const std::vector<net::Candidate> ours = std::move(gathering).get();

    bool lent = false;

    for (const net::Candidate& one : ours)
        if (one.sort == net::Candidate::Sort::lent) {
            lent = true;

            SAME(one.where.text(), std::string{"203.0.113.5"});
            SAME(one.where.port(), static_cast<std::uint16_t>(49152));
        }

    OUGHT(lent);

    // The lent path stands last: the own interface before the reflection, the reflection before the
    // relay.
    SAME(ours.back().sort, net::Candidate::Sort::lent);

    SAME(left.candidates().size(), ours.size());
}

TRIAL("Peer: without a relay there is no lent path and no talking through a relay") {
    net::Peer left{standing()};

    for (const net::Candidate& one : left.gather(net::StunClient{}, soon).get())
        OUGHT(one.sort != net::Candidate::Sort::lent);

    OUGHT(!left.relayed());
}

TRIAL("Peer: a permission goes by address, without the port") {
    net::Udp server = standing();

    net::Peer left{standing(), net::Relay{standing(), server.mine()}};

    auto gathering = left.gather(net::StunClient{}, soon);

    (void)allotted(server, soon);

    (void)std::move(gathering).get();

    left.add(net::Candidate{net::Address{"198.51.100.77", 5555}, net::Candidate::Sort::here});

    auto reaching = left.reach(net::Patience{3000});

    const net::Datagram came = server.receive(1024, soon).get();

    net::Message told;

    OUGHT(net::understood(came.bytes, told));
    SAME(static_cast<int>(told.task), static_cast<int>(net::Task::permission));
    SAME(told.peer.text(), std::string{"198.51.100.77"});

    // A permission covers the address, not the port: the port that the NAT gives the other side
    // will be different from the one in signaling anyway.
    SAME(told.peer.port(), static_cast<std::uint16_t>(0));

    server.send(came.from, headed(0x0108, came.bytes), soon).get();

    try {
        (void)std::move(reaching).get();
    } catch (const std::exception&) {
    }
}

TRIAL("Peer: through a relay it answers on the same channel and talks on it") {
    net::Udp server = standing();

    net::Peer left{standing(), net::Relay{standing(), server.mine()}};

    auto gathering = left.gather(net::StunClient{}, soon);

    const net::Address whence = allotted(server, soon);

    (void)std::move(gathering).get();

    const net::Address them{"198.51.100.77", 5555};

    left.add(net::Candidate{them, net::Candidate::Sort::here});

    auto reaching = left.reach(net::Patience{10000});

    // The permission for the other side.
    net::Datagram came = server.receive(1024, soon).get();

    net::Message told;

    OUGHT(net::understood(came.bytes, told));
    SAME(static_cast<int>(told.task), static_cast<int>(net::Task::permission));

    server.send(whence, headed(0x0108, came.bytes), soon).get();

    // The other side knocks on the lent address, so its check comes through the relay.
    net::Number theirs{};

    for (int which = 0; which < 12; ++which) theirs[which] = static_cast<std::byte>(100 + which);

    server.send(whence, carrying(them, knocking(theirs)), soon).get();

    // The response must go out on the same channel, and together with it our own check.
    bool answered = false;

    bool asked = false;

    net::Number number{};

    for (const auto latest = std::chrono::steady_clock::now() + std::chrono::seconds{20};
         !(answered && asked) && std::chrono::steady_clock::now() < latest;) {
        came = server.receive(1024, soon).get();

        if (!net::understood(came.bytes, told)) continue;

        if (told.task != net::Task::sending) continue;

        SAME(told.peer, them);

        net::Message inside;

        if (!net::understood(told.data, inside)) continue;

        if (inside.sort == net::Sort::told) {
            answered = true;

            SAME(inside.number, theirs);
        }

        if (inside.sort == net::Sort::asking) {
            asked = true;

            number = inside.number;
        }
    }

    OUGHT(answered);
    OUGHT(asked);

    // Their response to our check, also through the relay, completes the hole punch.
    server.send(whence, carrying(them, answering(number)), soon).get();

    SAME(std::move(reaching).get(), them);

    SAME(left.met(), them);
    OUGHT(left.relayed());

    // From now on we talk through the relay, not directly.
    auto waiting = left.receive(1024, soon);

    left.send(bytes("przez cudze pasmo"), soon).get();

    bool sent = false;

    for (const auto latest = std::chrono::steady_clock::now() + std::chrono::seconds{20};
         !sent && std::chrono::steady_clock::now() < latest;) {
        came = server.receive(1024, soon).get();

        if (!net::understood(came.bytes, told)) continue;

        if (told.task != net::Task::sending) continue;

        if (text(told.data) == "przez cudze pasmo") sent = true;
    }

    OUGHT(sent);

    server.send(whence, carrying(them, bytes("z drugiej strony")), soon).get();

    SAME(text(std::move(waiting).get()), std::string{"z drugiej strony"});
}

TRIAL("Peer: an empty one does not fall over when asked") {
    net::Peer left;

    OUGHT(left.candidates().empty());
    OUGHT(left.theirs().empty());
    OUGHT(!left.met().held());
    OUGHT(!left.relayed());

    left.add(net::Candidate{net::Address{"203.0.113.9", 5000}, net::Candidate::Sort::here});

    OUGHT(left.theirs().empty());
}

TRIAL("Channel: the header is recognized by two bits, not by guessing") {
    std::uint16_t channel = 0;

    Bytes data;

    // A STUN message has these bits zero and is not a channel.
    OUGHT(!net::unwrapped(net::asked(net::numbered()), channel, data));

    // Too short for the header alone.
    OUGHT(!net::unwrapped(Bytes{std::byte{0x40}, std::byte{0x01}}, channel, data));

    // Truncated content: the length promises more than arrived.
    Bytes cut = net::wrapped(0x4001, bytes("cztery"));

    cut.pop_back();

    OUGHT(!net::unwrapped(cut, channel, data));

    const Bytes whole = net::wrapped(0x4001, bytes("cztery"));

    // Four bytes of header and not a single byte of padding: over UDP the datagram marks its own
    // end.
    SAME(whole.size(), static_cast<std::size_t>(10));

    OUGHT(net::unwrapped(whole, channel, data));

    SAME(channel, static_cast<std::uint16_t>(0x4001));
    SAME(text(data), std::string{"cztery"});
}

TRIAL("Relay: the channel is requested by itself, and then the header has four bytes") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::Relay relay{mine, server.mine()};

    const net::Address them{"198.51.100.77", 5555};

    const Ready ready = readied(relay, server, them);

    OUGHT(ready.channel >= 0x4000);
    OUGHT(ready.channel <= 0x7FFF);

    bool tight = false;

    for (const auto latest = std::chrono::steady_clock::now() + std::chrono::seconds{20};
         !tight && std::chrono::steady_clock::now() < latest;) {
        relay.send(them, bytes("dwa"), soon).get();

        try {
            const net::Datagram came = server.receive(1024, net::Patience{200}).get();

            std::uint16_t channel = 0;

            Bytes data;

            if (!net::unwrapped(came.bytes, channel, data)) continue;

            SAME(channel, ready.channel);
            SAME(text(data), std::string{"dwa"});

            // Three bytes of content and four of header, not thirty-nine.
            SAME(came.bytes.size(), static_cast<std::size_t>(7));

            tight = true;
        } catch (const Overdue&) {
        }
    }

    OUGHT(tight);
}

TRIAL("Relay: content from a channel comes with the peer's address") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::Relay relay{mine, server.mine()};

    const net::Address them{"198.51.100.77", 5555};

    const Ready ready = readied(relay, server, them);

    auto waiting = relay.receive(1024, soon);

    server.send(ready.whence, channelling(ready.channel, "z kanału"), soon).get();

    const net::Datagram given = std::move(waiting).get();

    SAME(given.from, them);
    SAME(text(given.bytes), std::string{"z kanału"});
}

// The channel number is assigned by us, so a foreign number must not mean anything; it is not even
// known whom the sender would belong to.
TRIAL("Relay: a foreign channel number does not enter the conversation") {
    net::Udp mine = standing();
    net::Udp server = standing();

    net::Relay relay{mine, server.mine()};

    const net::Address them{"198.51.100.77", 5555};

    const Ready ready = readied(relay, server, them);

    server.send(ready.whence, channelling(static_cast<std::uint16_t>(ready.channel + 1), "obce"),
                soon)
        .get();

    bool late = false;

    try {
        (void)relay.receive(1024, net::Patience{600}).get();
    } catch (const Overdue&) {
        late = true;
    }

    OUGHT(late);
}
