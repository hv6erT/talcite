#include "proving.hpp"

#include "faking.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "talcite/address.hpp"
#include "talcite/listener.hpp"
#include "talcite/tls.hpp"
#include "talcite/stream.hpp"
#include "talcite/time.hpp"

using namespace talcite;

namespace {

const net::Patience soon{15000};



using faking::Watching;

// A listener on an IPv4 loopback address with a port chosen by the system.
net::Listener standing() { return net::Listener{net::Address::loopback(net::Family::four, 0)}; }

struct Pair {
    net::TlsStream server;
    net::TlsStream client;
};

// Connects to `gate`, runs the TLS handshake through `guard` on both ends at once, and returns the
// server and client streams.
Pair guarded(const net::Listener& gate, const talcite::Seam<net::TlsProvider>& guard) {
    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(gate.mine(), soon);

    net::Stream one = meeting.await();
    net::Stream two = joining.await();

    auto there = net::TlsStream::server(std::move(one), guard, soon);
    auto here = net::TlsStream::client(std::move(two), guard, "gdziekolwiek", soon);

    net::TlsStream server = there.await();
    net::TlsStream client = here.await();

    return Pair{std::move(server), std::move(client)};
}

}

TRIAL("Seam: the handshake completes on both sides") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    ought(both.server.held());
    ought(both.client.held());

    same(both.client.peer().port(), gate.mine().port());
}

TRIAL("Seam: content passes both ways") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    both.client.write(bytes("tajna wiadomość"), soon).await();

    same(text(both.server.read(1024, soon).await()), std::string{"tajna wiadomość"});

    both.server.write(bytes("i odpowiedź"), soon).await();

    same(text(both.client.read(1024, soon).await()), std::string{"i odpowiedź"});
}

TRIAL("Seam: a hundred thousand bytes pass in pieces") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    const std::string much(100000, 'z');

    auto writing = both.client.write(bytes(much), soon);

    std::string made;

    while (made.size() < much.size()) {
        const Bytes one = both.server.read(65536, soon).await();

        if (one.empty()) break;

        made += text(one);
    }

    writing.await();

    same(made.size(), much.size());
    ought(made == much);
}

TRIAL("Seam: reading less than a record leaves the rest for later") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    both.client.write(bytes("abcdefghij"), soon).await();

    same(text(both.server.read(4, soon).await()), std::string{"abcd"});
    same(text(both.server.read(4, soon).await()), std::string{"efgh"});
    same(text(both.server.read(4, soon).await()), std::string{"ij"});
}

TRIAL("Seam: the other side closing gives emptiness") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    both.client.close();

    same(static_cast<int>(both.server.read(64, soon).await().size()), 0);
}

TRIAL("Seam: silence ends with the deadline") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    bool late = false;

    try {
        (void)both.server.read(64, net::Patience{200}).await();
    } catch (const Overdue&) {
        late = true;
    }

    ought(late);
}

TRIAL("Seam: a second read at the same time is refused") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    auto first = both.server.read(64, net::Patience{300});

    bool busy = false;

    try {
        (void)both.server.read(64, net::Patience{300}).await();
    } catch (const std::exception&) {
        busy = true;
    }

    ought(busy);

    try {
        (void)first.await();
    } catch (const std::exception&) {
    }
}

TRIAL("Seam: trouble reported by the protection comes to the surface") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>(true)};

    Pair both = guarded(gate, guard);

    both.client.write(bytes("cokolwiek"), soon).await();

    bool amiss = false;

    try {
        (void)both.server.read(64, soon).await();
    } catch (const net::Trouble& why) {
        amiss = true;

        ought(std::string{why.what()}.find("Trucizna") != std::string::npos);
    }

    ought(amiss);
}

// Trouble that the adapter did not recognize is also `TlsTrouble`, with the reason "other"; on the
// server side without a name, because the server does not ask for one.
TRIAL("Seam: trouble without a recognized reason is TlsTrouble with reason other") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>(true)};

    Pair both = guarded(gate, guard);

    both.client.write(bytes("cokolwiek"), soon).await();

    bool amiss = false;

    try {
        (void)both.server.read(64, soon).await();
    } catch (const net::TlsTrouble& why) {
        amiss = true;

        same(why.why(), net::TlsTrouble::Why::other);
        ought(why.host().empty());
        ought(why.where().held());
    }

    ought(amiss);
}

// A handshake refusal with a reason from the adapter: each reason reaches the caller as it was,
// together with the server name the client asked for and the address of the other side.
TRIAL("Seam: a handshake refusal carries the reason, the server name and the address") {
    for (const net::TlsTrouble::Why one : {net::TlsTrouble::Why::ciphers, net::TlsTrouble::Why::untrusted,
                                           net::TlsTrouble::Why::expired, net::TlsTrouble::Why::misnamed}) {
        const net::Listener gate = standing();

        const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>(false, one)};

        auto meeting = gate.accept(soon);
        net::Stream two = net::Stream::connect(gate.mine(), soon).await();
        const net::Stream there = meeting.await();

        bool refused = false;

        try {
            (void)net::TlsStream::client(std::move(two), guard, "talcite.test", soon).await();
        } catch (const net::TlsTrouble& why) {
            refused = true;

            same(why.why(), one);
            same(why.host(), std::string{"talcite.test"});
            ought(why.where() == gate.mine());
            same(std::string{why.what()}, std::string{"Handshake rejected."});
        }

        ought(refused);
    }
}

TRIAL("Seam: a handshake cut off halfway rejects the promise") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    auto meeting = gate.accept(soon);
    auto joining = net::Stream::connect(gate.mine(), soon);

    net::Stream one = meeting.await();
    net::Stream two = joining.await();

    auto here = net::TlsStream::client(std::move(two), guard, "gdziekolwiek", soon);

    // The other side does not answer; it only closes.
    one.close();

    bool amiss = false;

    try {
        (void)here.await();
    } catch (const std::exception&) {
        amiss = true;
    }

    ought(amiss);
}

TRIAL("Seam: writing and reading at once do not mix on the socket") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    auto reading = both.client.read(1024, soon);

    both.server.write(bytes("od serwera"), soon).await();
    both.client.write(bytes("od klienta"), soon).await();

    same(text(reading.await()), std::string{"od serwera"});
    same(text(both.server.read(1024, soon).await()), std::string{"od klienta"});
}

// The guard is a value, so it may be absent. An empty one must say so plainly instead of reaching
// through a null pointer in the first round of the handshake.
class Nothing : public net::TlsProvider {
public:
    // The provider's name, as the seam reports it.
    [[nodiscard]] std::string_view name() const override { return "nothing"; }

    // Gives no client session, as if the provider could not make one.
    [[nodiscard]] std::unique_ptr<net::TlsSession> client(std::string) override { return nullptr; }

    // Gives no server session, as if the provider could not make one.
    [[nodiscard]] std::unique_ptr<net::TlsSession> server() override { return nullptr; }
};

// An empty seam means no provider, which is a talcite error, so it speaks talcite's language:
// `Unbound`, the same type that `Seam::provider()` throws. The promise rejects with it instead of
// throwing in the caller's face.
TRIAL("Seam: an empty seam refuses with Unbound, and provider() on it throws the same") {
    const net::Listener gate = standing();

    auto joining = net::Stream::connect(gate.mine(), soon);

    net::Stream two = joining.await();

    const talcite::Seam<net::TlsProvider> none;

    ought(!none.bound());

    bool amiss = false;

    try {
        (void)none.provider();
    } catch (const talcite::Unbound& why) {
        amiss = std::string{why.what()} == "No provider for TLS.";
    }

    ought(amiss);

    amiss = false;

    try {
        (void)net::TlsStream::client(std::move(two), none, "gdziekolwiek", soon).await();
    } catch (const talcite::Unbound&) {
        amiss = true;
    }

    ought(amiss);
}

TRIAL("Seam: the provider has a name, and the seam hands it out") {
    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    ought(guard.bound());
    same(std::string{guard.provider().name()}, std::string{"flipping"});
}

TRIAL("Seam: a guard that gives nothing refuses with a reason") {
    const net::Listener gate = standing();

    auto joining = net::Stream::connect(gate.mine(), soon);

    net::Stream two = joining.await();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Nothing>()};

    ought(guard.bound());
    ought(guard.provider().server() == nullptr);

    bool amiss = false;

    try {
        (void)net::TlsStream::server(std::move(two), guard, soon).await();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    ought(amiss);
}

// Rejections from `TlsStream` come out as trouble, not as a bare string: `reject` would wrap a
// string in `Rejected`, which `catch (const Trouble&)` does not catch. The rest of the library
// rejects with trouble, and these four places must do the same.
TRIAL("Seam: rejections come out as trouble, not as a bare string") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    both.client.close();

    bool amiss = false;

    try {
        (void)both.client.read(1024, soon).await();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    ought(amiss);

    amiss = false;

    try {
        both.client.write(bytes("do zamkniętego"), soon).await();
    } catch (const net::Trouble&) {
        amiss = true;
    }

    ought(amiss);
}

// A second read in flight must refuse with trouble, like everything else. The pair has to be fresh
// and open here: on a closed one the first read ends at once, the flag goes back, and the second
// finds it free, so the trial would pass by chance, once in a dozen runs checking something other
// than its name says.
TRIAL("Seam: a second read at the same time refuses with trouble") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    Pair both = guarded(gate, guard);

    // Nobody sends anything, so the first read waits out all its patience.
    auto reading = both.server.read(1024, net::Patience{1500});

    bool busy = false;

    try {
        (void)both.server.read(1024, net::Patience{300}).await();
    } catch (const net::Trouble&) {
        busy = true;
    }

    ought(busy);

    try {
        (void)reading.await();
    } catch (const std::exception&) {
    }
}

// Closing while writing. `TlsStream::close()` touches a member that a pool worker running the write
// reads at the same moment, and `Stream` holds a plain `shared_ptr`, not an atomic one. A write
// next to a read of the same pointer is UB, so the readers take a copy under the lock, and closing
// goes under the same lock.
//
// The trial does not check the result of the write; it may be rejected, and that is the point. It
// checks that nobody crashes or hangs, and under TSan it covers a path that no other trial touches.
TRIAL("Seam: closing during a write crashes nobody") {
    const net::Listener gate = standing();

    const talcite::Seam<net::TlsProvider> guard{std::make_shared<Watching>()};

    for (int turn = 0; turn < 8; ++turn) {
        Pair both = guarded(gate, guard);

        // More than goes out in one move: the write must still be running when the close comes.
        Bytes much;

        much.reserve(512 * 1024);

        for (std::size_t at = 0; at < 512 * 1024; ++at) much.push_back(std::byte{0x41});

        Promise<void> wrote = both.client.write(std::move(much), soon);

        wrote.start();

        std::thread other{[&both] { both.client.close(); }};

        try {
            wrote.await();
        } catch (...) {
        }

        other.join();

        both.server.close();
    }

    ought(true);
}
