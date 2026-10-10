#include "proving.hpp"

#include <chrono>
#include <cstddef>
#include <string>

#include "talcite/address.hpp"
#include "talcite/udp.hpp"
#include "talcite/time.hpp"

using namespace talcite;

namespace {

const net::Patience soon{4000};



// A UDP socket on an IPv4 loopback address with a port chosen by the system.
net::Udp standing() { return net::Udp{net::Address::loopback(net::Family::four, 0)}; }

}

TRIAL("Post: gets a port from the system") {
    const net::Udp one = standing();

    ought(one.held());
    ought(one.mine().port() != 0);
    same(one.mine().text(), std::string{"127.0.0.1"});
}

TRIAL("Post: an empty address is refused") {
    bool threw = false;

    try {
        const net::Udp one{net::Address{}};

        (void)one;
    } catch (const net::Trouble&) {
        threw = true;
    }

    ought(threw);
}

TRIAL("Post: a datagram arrives and knows who sent it") {
    const net::Udp here = standing();
    const net::Udp there = standing();

    auto waiting = here.receive(1024, soon);

    there.send(here.mine(), bytes("kartka"), soon).await();

    const net::Datagram made = waiting.await();

    same(text(made.bytes), std::string{"kartka"});
    same(made.from.port(), there.mine().port());
    same(made.from.text(), std::string{"127.0.0.1"});
}

TRIAL("Post: an empty datagram is not the same as no datagram") {
    const net::Udp here = standing();
    const net::Udp there = standing();

    auto waiting = here.receive(1024, soon);

    there.send(here.mine(), Bytes{}, soon).await();

    const net::Datagram made = waiting.await();

    same(static_cast<int>(made.bytes.size()), 0);
    ought(made.from.held());
}

TRIAL("Post: a truncated datagram reports trouble, not a piece") {
    const net::Udp here = standing();
    const net::Udp there = standing();

    auto waiting = here.receive(4, soon);

    there.send(here.mine(), bytes("dużo dłuższa treść"), soon).await();

    bool cut = false;

    try {
        (void)waiting.await();
    } catch (const net::Trouble& why) {
        cut = true;

        ought(std::string{why.what()}.find("buffer") != std::string::npos);
    }

    ought(cut);
}

TRIAL("Post: silence ends with the deadline") {
    const net::Udp here = standing();

    bool late = false;

    try {
        (void)here.receive(1024, net::Patience{80}).await();
    } catch (const Overdue&) {
        late = true;
    }

    ought(late);
}

TRIAL("Post: a second receive at the same time is refused") {
    const net::Udp here = standing();

    auto first = here.receive(1024, net::Patience{300});

    bool busy = false;

    try {
        (void)here.receive(1024, net::Patience{300}).await();
    } catch (const net::Trouble&) {
        busy = true;
    }

    ought(busy);

    try {
        (void)first.await();
    } catch (const Overdue&) {
    }
}

TRIAL("Post: an empty address is not a destination") {
    const net::Udp here = standing();

    bool amiss = false;

    try {
        here.send(net::Address{}, bytes("nigdzie"), soon).await();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    ought(amiss);
}

TRIAL("Post: several datagrams in a row keep their order on loopback") {
    const net::Udp here = standing();
    const net::Udp there = standing();

    for (int which = 0; which < 5; ++which) {
        auto waiting = here.receive(1024, soon);

        there.send(here.mine(), bytes(std::to_string(which)), soon).await();

        same(text(waiting.await().bytes), std::to_string(which));
    }
}

TRIAL("Post: the reply comes back to the sender's address") {
    const net::Udp here = standing();
    const net::Udp there = standing();

    auto waiting = here.receive(1024, soon);

    there.send(here.mine(), bytes("pytanie"), soon).await();

    const net::Datagram made = waiting.await();

    auto back = there.receive(1024, soon);

    here.send(made.from, bytes("odpowiedź"), soon).await();

    same(text(back.await().bytes), std::string{"odpowiedź"});
}

TRIAL("Post: a closed socket accepts no requests") {
    net::Udp here = standing();

    here.close();

    bool amiss = false;

    try {
        (void)here.receive(8, soon).await();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    ought(amiss);
}

TRIAL("Post: closing cancels a pending receive") {
    net::Udp here = standing();

    auto waiting = here.receive(1024, soon);

    here.close();

    bool amiss = false;

    try {
        (void)waiting.await();
    } catch (const net::Trouble&) {
        amiss = true;
    } catch (const Overdue&) {
        amiss = true;
    }

    ought(amiss);
}

// After closing, the socket answers everything politely, including the question about its own
// address.
TRIAL("Post: the address after closing is empty instead of blowing up") {
    net::Udp one{net::Address::loopback(net::Family::four, 0)};

    one.close();

    ought(!one.mine().held());
}

// A cancelled receive must free the socket; it is the same rule as in the stream, because the
// mechanism is shared.
TRIAL("Post: a cancelled receive frees the socket") {
    const net::Udp here = standing();
    const net::Udp there = standing();

    auto first = here.receive(1024, net::Patience{0});

    first.cancel();

    bool quit = false;

    try {
        (void)first.await();
    } catch (const Cancelled&) {
        quit = true;
    }

    ought(quit);

    there.send(here.mine(), bytes("paczka"), soon).await();

    for (int tried = 0; tried < 200; ++tried) {
        try {
            same(text(here.receive(1024, soon).await().bytes), std::string{"paczka"});

            return;
        } catch (const net::Trouble&) {
            Time::later(std::chrono::milliseconds{5}).await();
        }
    }

    ought(false);
}

// Cancelling frees the direction at once: the same rule and the same reason as for the listener
// and the stream, described in streaming.cpp. There is no wait in between, since a wait would hide
// the failure.
TRIAL("Post: after cancelling, receiving again works at once") {
    const net::Udp one = standing();
    const net::Udp other = standing();

    auto first = one.receive(64, net::Patience{0});

    first.cancel();

    try {
        (void)first.await();
    } catch (const std::exception&) {
    }

    auto waiting = one.receive(64, soon);

    other.send(one.mine(), bytes("znowu"), soon).await();

    same(text(waiting.await().bytes), std::string{"znowu"});
}

TRIAL("Post: a hundred cancels in a row do not leave it busy for good") {
    const net::Udp one = standing();
    const net::Udp other = standing();

    for (int again = 0; again < 100; ++again) {
        auto waiting = one.receive(64, net::Patience{0});

        waiting.cancel();

        try {
            (void)waiting.await();
        } catch (const std::exception&) {
        }
    }

    auto waiting = one.receive(64, soon);

    other.send(one.mine(), bytes("po stu"), soon).await();

    same(text(waiting.await().bytes), std::string{"po stu"});
}

TRIAL("Post: a local socket refuses, because there are no datagrams there") {
    bool told = false;

    try {
        const net::Udp one{net::Address::local("/tmp/talcite-poczta.sock")};
    } catch (const net::Trouble&) {
        told = true;
    }

    ought(told);
}
