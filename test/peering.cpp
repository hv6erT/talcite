#include "proving.hpp"

#include <cstddef>
#include <string>
#include <vector>

#include <chrono>

#include "talcite/address.hpp"
#include "talcite/peer.hpp"
#include "talcite/udp.hpp"
#include "talcite/stunclient.hpp"
#include "talcite/time.hpp"

using namespace talcite;

// There is no real NAT in the sandbox and it cannot be faked. So we check the protocol: whether
// both sides knocking at once reach each other, whether they pick the same path, whether data does
// not mix with the connectivity checks, and whether silence ends with the deadline. Getting through
// two real NATs can only be checked on a real network.
namespace {

const net::Patience soon{6000};

// A UDP socket on an IPv4 loopback address with a port chosen by the system.
net::Udp standing() { return net::Udp{net::Address::loopback(net::Family::four, 0)}; }


}

TRIAL("Peer: two sides knocking at once reach each other") {
    net::Udp one = standing();
    net::Udp two = standing();

    const net::Address where = one.mine();
    const net::Address there = two.mine();

    net::Peer left{one};
    net::Peer right{two};

    left.add(net::Candidate{there, net::Candidate::Sort::here});
    right.add(net::Candidate{where, net::Candidate::Sort::here});

    auto first = left.reach(soon);
    auto second = right.reach(soon);

    same(std::move(first).get(), there);
    same(std::move(second).get(), where);

    same(left.met(), there);
    same(right.met(), where);
}

TRIAL("Peer: the passive side answers and reaches its peer on its own") {
    net::Udp one = standing();
    net::Udp two = standing();

    net::Peer left{one};
    net::Peer right{two};

    left.add(net::Candidate{two.mine(), net::Candidate::Sort::here});

    // The right side knows the left one from signaling, but does not call reach itself. Its loop is
    // woken by waiting for data that nobody will send; it must answer the other side's checks and
    // reach its peer on its own. That is exactly what the side being knocked on looks like.
    right.add(net::Candidate{one.mine(), net::Candidate::Sort::here});

    auto waiting = right.receive(1024, net::Patience{2000});

    same(left.reach(soon).get(), two.mine());

    try {
        (void)std::move(waiting).get();
    } catch (const std::exception&) {
    }

    same(right.met(), one.mine());
}

// A STUN request put together byte by byte, to play a stranger who speaks up unasked.
Bytes knocking() {
    Bytes made(20, std::byte{0});

    made[1] = std::byte{0x01};
    made[4] = std::byte{0x21};
    made[5] = std::byte{0x12};
    made[6] = std::byte{0xA4};
    made[7] = std::byte{0x42};

    for (std::size_t at = 8; at < 20; ++at) made[at] = static_cast<std::byte>(at);

    return made;
}

// A NAT almost always gives the other side a different port from the one it managed to report. So
// a path from the same host may be added; without that, hole punching would almost never work.
TRIAL("Peer: another port of a known host is added by itself") {
    net::Udp one = standing();
    net::Udp two = standing();
    net::Udp three = standing();

    net::Peer left{one};

    // Signaling gave the port of the second socket, and the check comes from the third: the same
    // address, a different port, exactly as from behind a NAT.
    left.add(net::Candidate{two.mine(), net::Candidate::Sort::here});

    auto waking = left.receive(1024, net::Patience{1500});

    bool knows = false;

    for (int turn = 0; turn < 30 && !knows; ++turn) {
        three.send(one.mine(), knocking(), soon).get();

        later(std::chrono::milliseconds{50}).get();

        for (const net::Candidate& which : left.theirs())
            if (which.where == three.mine()) knows = true;
    }

    ought(knows);

    try {
        (void)std::move(waking).get();
    } catch (const std::exception&) {
    }
}

// Without this rule a single unsolicited request would be enough for a stranger to get onto the
// list of paths and receive a transaction number from us; the number is the only proof of a
// response's identity, so sending it back would count as a won hole punch.
TRIAL("Peer: a host nobody mentioned is not added") {
    net::Udp one = standing();
    net::Udp evil = standing();

    net::Peer left{one};

    // The only known path leads elsewhere and will never answer.
    left.add(net::Candidate{net::Address{"203.0.113.9", 5000}, net::Candidate::Sort::here});

    auto waking = left.receive(1024, net::Patience{900});

    for (int turn = 0; turn < 6; ++turn) {
        evil.send(one.mine(), knocking(), soon).get();

        later(std::chrono::milliseconds{60}).get();
    }

    ought(!left.met().held());

    same(static_cast<int>(left.theirs().size()), 1);

    for (const net::Candidate& which : left.theirs()) ought(!(which.where == evil.mine()));

    try {
        (void)std::move(waking).get();
    } catch (const std::exception&) {
    }
}

TRIAL("Peer: after punching through, the sides can talk") {
    net::Udp one = standing();
    net::Udp two = standing();

    net::Peer left{one};
    net::Peer right{two};

    left.add(net::Candidate{two.mine(), net::Candidate::Sort::here});
    right.add(net::Candidate{one.mine(), net::Candidate::Sort::here});

    auto first = left.reach(soon);
    auto second = right.reach(soon);

    (void)std::move(first).get();
    (void)std::move(second).get();

    auto waiting = right.receive(1024, soon);

    left.send(bytes("witaj po drugiej stronie dziury"), soon).get();

    same(text(std::move(waiting).get()), std::string{"witaj po drugiej stronie dziury"});
}

TRIAL("Peer: connectivity checks do not mix with data") {
    net::Udp one = standing();
    net::Udp two = standing();

    net::Peer left{one};
    net::Peer right{two};

    left.add(net::Candidate{two.mine(), net::Candidate::Sort::here});
    right.add(net::Candidate{one.mine(), net::Candidate::Sort::here});

    auto first = left.reach(soon);
    auto second = right.reach(soon);

    (void)std::move(first).get();
    (void)std::move(second).get();

    // Keepalives and responses travel over the same socket the whole time. Not a single STUN byte
    // may reach the data, or the caller would get garbage instead of its message.
    for (int turn = 0; turn < 5; ++turn) {
        auto waiting = right.receive(1024, soon);

        left.send(bytes("rata " + std::to_string(turn)), soon).get();

        same(text(std::move(waiting).get()), "rata " + std::to_string(turn));
    }
}

TRIAL("Peer: without a path there is nowhere to knock") {
    net::Udp one = standing();

    net::Peer left{one};

    bool amiss = false;

    try {
        (void)left.reach(soon).get();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    ought(amiss);
}

TRIAL("Peer: a path to nowhere ends with the deadline") {
    net::Udp one = standing();
    net::Udp two = standing();

    const net::Address nobody = two.mine();

    // The second socket is closed, so the address is valid but nobody stands behind it.
    two.close();

    net::Peer left{one};

    left.add(net::Candidate{nobody, net::Candidate::Sort::here});

    bool late = false;

    try {
        (void)left.reach(net::Patience{700}).get();
    } catch (const Overdue&) {
        late = true;
    }

    ought(late);
}

TRIAL("Peer: our own address is not taken as the other side's path") {
    net::Udp one = standing();

    net::Peer left{one};

    left.add(net::Candidate{one.mine(), net::Candidate::Sort::here});

    ought(left.theirs().empty());
}

TRIAL("Peer: paths are ordered by sort") {
    net::Udp one = standing();

    net::Peer left{one};

    left.add(net::Candidate{net::Address{"203.0.113.9", 5000}, net::Candidate::Sort::lent});
    left.add(net::Candidate{net::Address{"198.51.100.4", 4000}, net::Candidate::Sort::seen});
    left.add(net::Candidate{net::Address{"192.168.1.7", 3000}, net::Candidate::Sort::here});

    const std::vector<net::Candidate> made = left.theirs();

    same(static_cast<int>(made.size()), 3);
    same(made[0].sort, net::Candidate::Sort::here);
    same(made[1].sort, net::Candidate::Sort::seen);
    same(made[2].sort, net::Candidate::Sort::lent);
}

TRIAL("Peer: the same path does not go in twice") {
    net::Udp one = standing();

    net::Peer left{one};

    const net::Candidate where{net::Address{"192.168.1.7", 3000}, net::Candidate::Sort::here};

    left.add(where);
    left.add(where);

    same(static_cast<int>(left.theirs().size()), 1);
}

TRIAL("Peer: gathering candidates without a scout gives only host candidates") {
    net::Udp one{net::Address::any(net::Family::four, 0)};

    net::Peer left{one};

    const std::vector<net::Candidate> made = left.gather(net::StunClient{}, soon).get();

    for (const net::Candidate& which : made) {
        same(which.sort, net::Candidate::Sort::here);

        // The candidate carries the port of our socket, because that is what the other side looks
        // for.
        same(which.where.port(), one.mine().port());

        // Loopback must not go out: it would make the other side knock on itself, and a response
        // from its own socket would look like a successful punch.
        ought(!(which.where == net::Address::loopback(net::Family::four, which.where.port())));
    }

    same(left.candidates().size(), made.size());
}

TRIAL("Peer: there is nobody to talk to before punching through") {
    net::Udp one = standing();

    net::Peer left{one};

    bool amiss = false;

    try {
        left.send(bytes("za wcześnie"), soon).get();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    ought(amiss);
}

TRIAL("Peer: silence on receive ends with the deadline") {
    net::Udp one = standing();

    net::Peer left{one};

    bool late = false;

    try {
        (void)left.receive(1024, net::Patience{300}).get();
    } catch (const Overdue&) {
        late = true;
    }

    ought(late);
}

TRIAL("Peer: a closed one accepts nothing") {
    net::Udp one = standing();

    net::Peer left{one};

    left.close();

    bool amiss = false;

    try {
        (void)left.reach(soon).get();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    ought(amiss);
}

TRIAL("Peer: two knocks at once are refused") {
    net::Udp one = standing();
    net::Udp two = standing();

    net::Peer left{one};

    left.add(net::Candidate{two.mine(), net::Candidate::Sort::here});

    auto first = left.reach(net::Patience{500});

    bool busy = false;

    try {
        (void)left.reach(soon).get();
    } catch (const net::Trouble&) {
        busy = true;
    }

    ought(busy);

    try {
        (void)std::move(first).get();
    } catch (const std::exception&) {
    }
}

// A refusal must not eat the message: the size check comes before taking it off the queue, so a
// retry with more room still finds it.
TRIAL("Peer: an oversized message is not lost on refusal") {
    net::Udp one = standing();
    net::Udp two = standing();

    net::Peer left{one};
    net::Peer right{two};

    left.add(net::Candidate{two.mine(), net::Candidate::Sort::here});
    right.add(net::Candidate{one.mine(), net::Candidate::Sort::here});

    auto first = left.reach(soon);
    auto second = right.reach(soon);

    (void)std::move(first).get();
    (void)std::move(second).get();

    left.send(bytes("sto bajtów by tu nie weszło"), soon).get();

    bool amiss = false;

    for (int turn = 0; turn < 40 && !amiss; ++turn) {
        try {
            (void)right.receive(4, net::Patience{100}).get();
        } catch (const Overdue&) {
            continue;
        } catch (const net::Trouble&) {
            amiss = true;
        }
    }

    ought(amiss);

    same(text(right.receive(1024, soon).get()), std::string{"sto bajtów by tu nie weszło"});
}
