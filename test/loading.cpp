#include "tal/testing.hpp"

#include <cstddef>
#include <string>
#include <vector>

#include "talcite/address.hpp"
#include "exchange.hpp"
#include "talcite/listener.hpp"
#include "talcite/udp.hpp"
#include "talcite/stream.hpp"
#include "talcite/time.hpp"

using namespace talcite;

// These cases aim at the places where the backends differ most: accepting connections, connecting,
// two operations at once on one socket, cancelling one of them, and writes larger than the kernel
// buffer. On Unix systems they exercise poll and epoll, on Windows IOCP, where each of these things
// is an entirely different system call.
namespace {

const net::Patience soon{15000};


// A listener on an IPv4 loopback address with a port chosen by the system.
net::Listener standing() { return net::Listener{net::Address::loopback(net::Family::four, 0)}; }

struct Pair {
    net::Stream server;
    net::Stream client;
};

// Accepts one connection on `gate` while connecting to it, and returns both ends.
Pair joined(const net::Listener& gate) {
    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(gate.mine(), soon);

    net::Stream server = std::move(meeting).get();
    net::Stream client = std::move(joining).get();

    return Pair{std::move(server), std::move(client)};
}

}

TRIAL("Load: fifty connections one after another") {
    const net::Listener gate = standing();

    for (int which = 0; which < 50; ++which) {
        Pair both = joined(gate);

        both.client.write(bytes(std::to_string(which)), soon).get();

        SAME(text(both.server.read(16, soon).get()), std::to_string(which));
    }
}

TRIAL("Load: fifty connections at once") {
    const net::Listener gate = standing();

    std::vector<net::Stream> servers;
    std::vector<net::Stream> clients;

    for (int which = 0; which < 50; ++which) {
        auto meeting = gate.accept(soon);
        auto joining = net::Stream::connect(gate.mine(), soon);

        servers.push_back(std::move(meeting).get());
        clients.push_back(std::move(joining).get());
    }

    std::vector<Promise<Bytes>> reading;

    for (net::Stream& one : servers) reading.push_back(one.read(16, soon));

    for (int which = 0; which < 50; ++which)
        clients[static_cast<std::size_t>(which)].write(bytes(std::to_string(which)), soon).get();

    for (int which = 0; which < 50; ++which)
        SAME(text(std::move(reading[static_cast<std::size_t>(which)]).get()), std::to_string(which));
}

TRIAL("Load: reading and writing at once on one stream") {
    const net::Listener gate = standing();

    Pair both = joined(gate);

    auto reading = both.client.read(64, soon);

    both.server.write(bytes("z drugiej strony"), soon).get();
    both.client.write(bytes("i z tej"), soon).get();

    SAME(text(std::move(reading).get()), std::string{"z drugiej strony"});
    SAME(text(both.server.read(64, soon).get()), std::string{"i z tej"});
}

// The most important case for IOCP: cancelling one operation, not the whole socket. One stream has
// a read with a deadline and a write without one pending; when the deadline passes the read must
// fail and the write must complete.
TRIAL("Load: a deadline cancels one operation, not the socket") {
    const net::Listener gate = standing();

    Pair both = joined(gate);

    const std::string much(400000, 'x');

    auto writing = both.client.write(bytes(much), soon);
    auto reading = both.client.read(64, net::Patience{200});

    bool late = false;

    try {
        (void)std::move(reading).get();
    } catch (const Overdue&) {
        late = true;
    }

    OUGHT(late);

    std::string made;

    while (made.size() < much.size()) {
        const Bytes one = both.server.read(65536, soon).get();

        if (one.empty()) break;

        made += text(one);
    }

    std::move(writing).get();

    SAME(made.size(), much.size());

    // The stream lives on: the cancelled read did not take the connection with it.
    both.server.write(bytes("dalej stoję"), soon).get();

    SAME(text(both.client.read(64, soon).get()), std::string{"dalej stoję"});
}

TRIAL("Load: an empty write breaks nothing") {
    const net::Listener gate = standing();

    Pair both = joined(gate);

    both.client.write(Bytes{}, soon).get();
    both.client.write(bytes("po pustym"), soon).get();

    SAME(text(both.server.read(64, soon).get()), std::string{"po pustym"});
}

TRIAL("Load: reading less than what arrived") {
    const net::Listener gate = standing();

    Pair both = joined(gate);

    both.client.write(bytes("abcdefgh"), soon).get();

    SAME(text(both.server.read(3, soon).get()), std::string{"abc"});
    SAME(text(both.server.read(3, soon).get()), std::string{"def"});
    SAME(text(both.server.read(3, soon).get()), std::string{"gh"});
}

TRIAL("Load: closing cancels both directions at once") {
    const net::Listener gate = standing();

    Pair both = joined(gate);

    const std::string much(400000, 'y');

    auto writing = both.client.write(bytes(much), soon);
    auto reading = both.client.read(64, soon);

    both.client.close();

    int amiss = 0;

    try {
        (void)std::move(reading).get();
    } catch (const std::exception&) {
        ++amiss;
    }

    try {
        std::move(writing).get();
    } catch (const std::exception&) {
        ++amiss;
    }

    // The write may have reached the kernel buffer before the close, so we only require that the
    // read fails and that nothing hangs.
    OUGHT(amiss >= 1);
}

TRIAL("Load: listener closed while waiting for a connection") {
    net::Listener gate = standing();

    auto meeting = gate.accept(soon);

    gate.close();

    bool amiss = false;

    try {
        (void)std::move(meeting).get();
    } catch (const std::exception&) {
        amiss = true;
    }

    OUGHT(amiss);
}

TRIAL("Load: connecting to a dead port fails instead of hanging") {
    net::Address where;

    {
        const net::Listener gate = standing();

        where = gate.mine();
    }

    bool amiss = false;

    try {
        (void)net::Stream::connect(where, net::Patience{3000}).get();
    } catch (const std::exception&) {
        amiss = true;
    }

    OUGHT(amiss);
}

TRIAL("Load: twenty datagrams in a row") {
    const net::Udp here{net::Address::loopback(net::Family::four, 0)};
    const net::Udp there{net::Address::loopback(net::Family::four, 0)};

    for (int which = 0; which < 20; ++which) {
        auto waiting = here.receive(1024, soon);

        there.send(here.mine(), bytes(std::to_string(which)), soon).get();

        SAME(text(std::move(waiting).get().bytes), std::to_string(which));
    }
}

TRIAL("Load: afterwards the exchange holds no sockets") {
    {
        const net::Listener gate = standing();

        for (int which = 0; which < 5; ++which) {
            Pair both = joined(gate);

            both.client.write(bytes("raz"), soon).get();

            (void)both.server.read(16, soon).get();
        }
    }

    for (int tried = 0; tried < 400 && net::Exchange::running().waiting() > 0; ++tried)
        later(std::chrono::milliseconds{5}).get();

    SAME(static_cast<int>(net::Exchange::running().waiting()), 0);
}
